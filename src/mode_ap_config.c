#include "mode_ap_config.h"

#include "common.h"
#include "led_status.h"
#include "storage.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

// dhcpserver.h / dnsserver.h 不是 pico-sdk 內建函式庫，是 pico-examples 的共用工具程式，
// 需從 pico-examples/pico_w/wifi/dhcpserver 與 .../dnsserver 複製到專案根目錄。
// 詳見 CMakeLists.txt 底部註解。
#include "dhcpserver.h"
#include "dnsserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_SSID "PicoGateway-Setup"
// TODO: 正式版建議改成每台裝置唯一密碼（例如印在裝置貼紙上），不要硬編在原始碼裡。
#define AP_PASSWORD "gateway123"
#define HTTP_PORT 80
#define HTTP_RX_BUF_SIZE 1024

static const char CONFIG_FORM_HTML[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>中繼裝置設定</title></head><body>"
    "<h2>中繼裝置設定</h2>"
    "<form method='POST' action='/save'>"
    "WiFi SSID: <input name='ssid'><br>"
    "WiFi 密碼: <input name='pass' type='password'><br>"
    "個案姓名: <input name='pname'><br>"
    "個案編號: <input name='pid'><br>"
    "個管師資訊: <input name='cm'><br>"
    "<button type='submit'>儲存並重新啟動</button>"
    "</form></body></html>";

static const char SAVED_RESPONSE_HTML[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><body><h2>設定已儲存，裝置即將切換到藍芽接收模式。</h2></body></html>";

typedef struct {
    char buf[HTTP_RX_BUF_SIZE];
    uint16_t len;
} http_conn_state_t;

static device_config_t s_pending_config;
static volatile bool s_config_submitted = false;

static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

// 從 application/x-www-form-urlencoded 格式的 body 取出指定欄位的值（已 URL-decode）。
static bool extract_field(const char *body, const char *key, char *out, size_t out_size) {
    size_t key_len = strlen(key);
    const char *p = body;

    while ((p = strstr(p, key)) != NULL) {
        bool at_field_start = (p == body) || (*(p - 1) == '&');
        if (at_field_start && p[key_len] == '=') {
            const char *value_start = p + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            size_t value_len = value_end ? (size_t)(value_end - value_start) : strlen(value_start);

            char raw[256];
            if (value_len >= sizeof(raw)) {
                value_len = sizeof(raw) - 1;
            }
            memcpy(raw, value_start, value_len);
            raw[value_len] = '\0';

            url_decode(raw, out, out_size);
            return true;
        }
        p += key_len;
    }
    return false;
}

static void parse_and_store_form(const char *body) {
    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    extract_field(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    extract_field(body, "pass", cfg.wifi_password, sizeof(cfg.wifi_password));
    extract_field(body, "pname", cfg.patient_name, sizeof(cfg.patient_name));
    extract_field(body, "pid", cfg.patient_id, sizeof(cfg.patient_id));
    extract_field(body, "cm", cfg.case_manager_info, sizeof(cfg.case_manager_info));
    cfg.valid = true;

    s_pending_config = cfg;
    s_config_submitted = true;
    printf("[AP_CONFIG] form submitted, ssid=\"%s\"\n", cfg.wifi_ssid);
}

static const char *find_http_body(const char *req, uint16_t len) {
    const char sep[] = "\r\n\r\n";
    for (uint16_t i = 0; i + 4 <= len; i++) {
        if (memcmp(req + i, sep, 4) == 0) {
            return req + i + 4;
        }
    }
    return NULL;
}

// 這個模組只設計給單一手機連線設定使用（同一時間一個連線），不是通用多連線 HTTP server。
static err_t http_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)err;
    http_conn_state_t *state = (http_conn_state_t *)arg;

    if (p == NULL) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    uint16_t space_left = sizeof(state->buf) - state->len - 1;
    uint16_t copy_len = p->tot_len < space_left ? p->tot_len : space_left;
    pbuf_copy_partial(p, state->buf + state->len, copy_len, 0);
    state->len += copy_len;
    state->buf[state->len] = '\0';
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    if (strstr(state->buf, "\r\n\r\n") == NULL) {
        return ERR_OK; // 表頭還沒收完，繼續等下一段
    }

    if (strncmp(state->buf, "POST", 4) == 0) {
        const char *body = find_http_body(state->buf, state->len);
        if (body != NULL) {
            parse_and_store_form(body);
        }
        tcp_write(tpcb, SAVED_RESPONSE_HTML, sizeof(SAVED_RESPONSE_HTML) - 1, TCP_WRITE_FLAG_COPY);
    } else {
        tcp_write(tpcb, CONFIG_FORM_HTML, sizeof(CONFIG_FORM_HTML) - 1, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
    tcp_close(tpcb);
    return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    (void)err;
    static http_conn_state_t state; // 單一連線；下一個連線進來會重置覆蓋
    memset(&state, 0, sizeof(state));
    tcp_arg(newpcb, &state);
    tcp_recv(newpcb, http_recv_cb);
    return ERR_OK;
}

void mode_ap_config_run(void) {
    s_config_submitted = false;

    printf("[AP_CONFIG] starting hotspot ssid=\"%s\"\n", AP_SSID);
    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, &cyw43_state.netif[CYW43_ITF_AP], &gw, &mask);

    dns_server_t dns_server;
    dns_server_init(&dns_server, &cyw43_state.netif[CYW43_ITF_AP], &gw);

    struct tcp_pcb *listen_pcb = tcp_new();
    tcp_bind(listen_pcb, IP_ANY_TYPE, HTTP_PORT);
    listen_pcb = tcp_listen(listen_pcb);
    tcp_accept(listen_pcb, http_accept_cb);

    while (!s_config_submitted) {
        led_status_poll();
        sleep_ms(20);
    }

    storage_save_config(&s_pending_config);

    tcp_close(listen_pcb);
    dns_server_deinit(&dns_server);
    dhcp_server_deinit(&dhcp_server);
    cyw43_arch_disable_ap_mode();
}
