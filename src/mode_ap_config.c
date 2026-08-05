#include "mode_ap_config.h"

#include "common.h"
#include "display_status.h"
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_SSID "PicoGateway-Setup"
// TODO: 正式版建議改成每台裝置唯一密碼（例如印在裝置貼紙上），不要硬編在原始碼裡。
#define AP_PASSWORD "gateway123"
#define HTTP_PORT 80
// 真實手機瀏覽器（尤其 iOS 彈出的 captive portal 內嵌瀏覽器）送出的請求標頭
// 一般比簡單測試工具大很多，1024 bytes 實測會把表單 body 截斷。
#define HTTP_RX_BUF_SIZE 4096

#define WIFI_SCAN_TIMEOUT_MS   8000
#define MAX_SCANNED_NETWORKS   15
#define CONFIG_FORM_BUF_SIZE   4096

typedef struct {
    char ssid[33];
    int16_t rssi;
} scanned_network_t;

static scanned_network_t s_scanned[MAX_SCANNED_NETWORKS];
static int s_scanned_count = 0;
static char s_config_form_response[CONFIG_FORM_BUF_SIZE];
static size_t s_config_form_response_len = 0;

// 進入設定模式時讀出來的現有設定：一來用來把「目前的設定」顯示回表單上，
// 二來讓密碼欄位留空時可以判斷「使用者是不想改密碼」而不是「要清空密碼」。
static device_config_t s_current_config;
static bool s_have_current_config = false;

// 邊界安全的字串附加：buf_size 內一定會留一個 '\0' 的位置，超過的部分直接捨棄。
static void html_append(char *buf, size_t buf_size, size_t *len, const char *fmt, ...) {
    if (*len + 1 >= buf_size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *len, buf_size - *len, fmt, args);
    va_end(args);
    if (n > 0) {
        size_t written = (size_t)n;
        size_t space = buf_size - *len - 1;
        *len += (written < space) ? written : space;
    }
}

// 掃到的 SSID 可能含有 <, >, &, " 這類字元（附近隨便一個惡意熱點就能故意取這種
// 名稱），直接嵌進 HTML 會被當成標籤/跳出屬性，是 XSS 風險，一律轉成安全的實體。
static void html_append_escaped(char *buf, size_t buf_size, size_t *len, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
            case '<':  html_append(buf, buf_size, len, "&lt;"); break;
            case '>':  html_append(buf, buf_size, len, "&gt;"); break;
            case '&':  html_append(buf, buf_size, len, "&amp;"); break;
            case '"':  html_append(buf, buf_size, len, "&quot;"); break;
            case '\'': html_append(buf, buf_size, len, "&#39;"); break;
            default:   html_append(buf, buf_size, len, "%c", *p); break;
        }
    }
}

static int wifi_scan_result_cb(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (result == NULL || result->ssid_len == 0 || result->ssid_len > 32) {
        return 0; // 忽略隱藏（空名稱）網路
    }

    char ssid[33];
    memcpy(ssid, result->ssid, result->ssid_len);
    ssid[result->ssid_len] = '\0';

    // 同一個 SSID 可能有多顆基地台（訊號延伸器），已經看過就只留訊號較強的一筆。
    for (int i = 0; i < s_scanned_count; i++) {
        if (strcmp(s_scanned[i].ssid, ssid) == 0) {
            if (result->rssi > s_scanned[i].rssi) {
                s_scanned[i].rssi = result->rssi;
            }
            return 0;
        }
    }

    if (s_scanned_count < MAX_SCANNED_NETWORKS) {
        strcpy(s_scanned[s_scanned_count].ssid, ssid);
        s_scanned[s_scanned_count].rssi = result->rssi;
        s_scanned_count++;
    }
    return 0;
}

// 進入設定模式時先掃一次附近的 WiFi，讓設定頁面用下拉選單列出來，使用者不用
// 自己手動打 SSID（隱藏網路掃不到，仍可以用下面的手動輸入框填）。
static void scan_nearby_wifi(void) {
    s_scanned_count = 0;

    // 掃描要在 station 介面真的啟用之後才會有結果，cyw43_arch_enable_sta_mode()
    // 是 void，呼叫端拿不到成功/失敗訊號；實測發現剛從藍牙模式切過來的那一刻，
    // 底層 cyw43_wifi_on() 偶爾還沒真的把介面帶起來（itf_state 還是 0），
    // 導致 cyw43_wifi_scan() 回傳 -CYW43_EPERM(-4)。用重試代替假設它一定成功。
    for (int attempt = 0; attempt < 10; attempt++) {
        cyw43_arch_enable_sta_mode();
        if (cyw43_state.itf_state & (1 << CYW43_ITF_STA)) {
            break;
        }
        sleep_ms(100);
    }

    printf("[AP_CONFIG] scanning nearby WiFi...\n");
    cyw43_wifi_scan_options_t scan_options;
    memset(&scan_options, 0, sizeof(scan_options));
    int err = cyw43_wifi_scan(&cyw43_state, &scan_options, NULL, wifi_scan_result_cb);
    if (err != 0) {
        printf("[AP_CONFIG] cyw43_wifi_scan failed (err=%d)\n", err);
        cyw43_arch_disable_sta_mode();
        return;
    }

    absolute_time_t deadline = make_timeout_time_ms(WIFI_SCAN_TIMEOUT_MS);
    while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(deadline)) {
        sleep_ms(50);
    }
    printf("[AP_CONFIG] scan done, found %d network(s)\n", s_scanned_count);

    // 掃描完就關掉 station 介面——馬上要開的是 AP 熱點，同一時間只維持一種
    // WiFi 用途，跟專案裡「單一無線擁有者」的一貫原則一致。
    cyw43_arch_disable_sta_mode();
}

static void render_config_form_response(void) {
    char *buf = s_config_form_response;
    size_t size = sizeof(s_config_form_response);
    size_t len = 0;

    // 先把 body 組出來，才能算出正確的 Content-Length。
    char body[CONFIG_FORM_BUF_SIZE - 128];
    size_t body_len = 0;

    // 有沒有現有設定：顯示回表單上，讓使用者知道目前是什麼設定、只改要改的欄位。
    const bool has_cfg = s_have_current_config;
    const char *cur_ssid = has_cfg ? s_current_config.wifi_ssid : "";
    bool ssid_in_scan_list = false;
    for (int i = 0; i < s_scanned_count; i++) {
        if (strcmp(s_scanned[i].ssid, cur_ssid) == 0) {
            ssid_in_scan_list = true;
            break;
        }
    }

    html_append(body, sizeof(body), &body_len,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>中繼裝置設定</title></head><body>"
        "<h2>中繼裝置設定</h2>");

    if (has_cfg) {
        html_append(body, sizeof(body), &body_len,
            "<p>目前設定：WiFi「");
        html_append_escaped(body, sizeof(body), &body_len, cur_ssid);
        html_append(body, sizeof(body), &body_len, "」，個案「");
        html_append_escaped(body, sizeof(body), &body_len, s_current_config.patient_name);
        html_append(body, sizeof(body), &body_len, "」（編號 ");
        html_append_escaped(body, sizeof(body), &body_len, s_current_config.patient_id);
        html_append(body, sizeof(body), &body_len, "）。以下欄位已帶入目前的值，只改要改的部分即可。</p>");
    }

    html_append(body, sizeof(body), &body_len,
        "<form method='POST' action='/save'>"
        // 不依賴 JavaScript：下拉選單本身就是 ssid 欄位，手動輸入是另一個獨立的
        // 覆寫欄位。手機連上熱點後自動彈出的「登入頁」瀏覽器有些不支援/限制
        // JavaScript，之前用 onchange 把下拉選單值複製到文字框的做法在那種
        // 環境會整個失效，送出的 ssid 永遠是空字串。
        "WiFi 名稱（選附近掃到的）: <select name='ssid'>"
        "<option value=''>-- 請選擇，或改用下面手動輸入 --</option>");

    for (int i = 0; i < s_scanned_count; i++) {
        html_append(body, sizeof(body), &body_len, "<option value='");
        html_append_escaped(body, sizeof(body), &body_len, s_scanned[i].ssid);
        html_append(body, sizeof(body), &body_len, "'%s>",
            (has_cfg && strcmp(s_scanned[i].ssid, cur_ssid) == 0) ? " selected" : "");
        html_append_escaped(body, sizeof(body), &body_len, s_scanned[i].ssid);
        html_append(body, sizeof(body), &body_len, " (%d dBm)</option>", s_scanned[i].rssi);
    }

    html_append(body, sizeof(body), &body_len, "</select><br>"
        "手動輸入 WiFi 名稱（若上面沒有你要的網路，填這裡會蓋掉上面的選擇）: "
        "<input name='ssid_manual' value='");
    // 目前的 SSID 沒有出現在掃描結果裡（隱藏網路，或現場掃不到）才帶進手動欄位，
    // 避免使用者選了下拉選單裡別的網路，卻被這裡沒清空的舊值蓋掉。
    if (has_cfg && !ssid_in_scan_list) {
        html_append_escaped(body, sizeof(body), &body_len, cur_ssid);
    }
    html_append(body, sizeof(body), &body_len, "'><br>"
        "WiFi 密碼: <input name='pass' type='password' placeholder='%s'><br>"
        "個案姓名: <input name='pname' value='",
        has_cfg ? "留空 = 不變更目前密碼" : "");
    if (has_cfg) {
        html_append_escaped(body, sizeof(body), &body_len, s_current_config.patient_name);
    }
    html_append(body, sizeof(body), &body_len, "'><br>個案編號: <input name='pid' value='");
    if (has_cfg) {
        html_append_escaped(body, sizeof(body), &body_len, s_current_config.patient_id);
    }
    html_append(body, sizeof(body), &body_len, "'><br>個管師資訊: <input name='cm' value='");
    if (has_cfg) {
        html_append_escaped(body, sizeof(body), &body_len, s_current_config.case_manager_info);
    }
    html_append(body, sizeof(body), &body_len,
        "'><br>"
        "<button type='submit'>儲存並重新啟動</button>"
        "</form></body></html>");

    html_append(buf, size, &len,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        (unsigned)body_len, body);

    s_config_form_response_len = len;
}

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
    // 除錯用：直接印出收到的原始 body，避免再猜測解析邏輯到底出了什麼問題。
    printf("[AP_CONFIG] raw form body (%u bytes): \"%s\"\n", (unsigned)strlen(body), body);

    device_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    extract_field(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));

    // 手動輸入欄位非空就蓋掉下拉選單的選擇（給掃描不到的隱藏網路用）。
    char ssid_manual[WIFI_SSID_MAX_LEN];
    ssid_manual[0] = '\0';
    extract_field(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    if (ssid_manual[0] != '\0') {
        strncpy(cfg.wifi_ssid, ssid_manual, sizeof(cfg.wifi_ssid) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    }

    extract_field(body, "pass", cfg.wifi_password, sizeof(cfg.wifi_password));
    if (cfg.wifi_password[0] == '\0' && s_have_current_config) {
        // 密碼欄位留空：視為「不變更」，沿用目前已存的密碼，而不是把密碼清空。
        strncpy(cfg.wifi_password, s_current_config.wifi_password, sizeof(cfg.wifi_password) - 1);
        cfg.wifi_password[sizeof(cfg.wifi_password) - 1] = '\0';
    }
    extract_field(body, "pname", cfg.patient_name, sizeof(cfg.patient_name));
    extract_field(body, "pid", cfg.patient_id, sizeof(cfg.patient_id));
    extract_field(body, "cm", cfg.case_manager_info, sizeof(cfg.case_manager_info));
    cfg.valid = true;

    s_pending_config = cfg;
    s_config_submitted = true;
    printf("[AP_CONFIG] form submitted, ssid=\"%s\"\n", cfg.wifi_ssid);
}

static const char REDIRECT_TO_ROOT_HTTP[] =
    "HTTP/1.1 302 Found\r\n"
    "Location: http://192.168.4.1/\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n";

// 手機/電腦連上熱點後，作業系統會先偷偷發一個「連通性檢測」請求（例如 Android
// 打 /generate_204、iOS 打 /hotspot-detect.html、Windows 打 /connecttest.txt）
// 到固定的網域，藉此判斷這個 WiFi 是不是需要另外登入的「強制網頁」（captive
// portal）。dns_server.c 已經把所有網域查詢都導回這台裝置自己的 IP，這裡再對
// 這些檢測請求一律回 302 導回 "/"，作業系統看到跟預期不符（不是純 204/特定
// 字串）就會自動跳出登入頁瀏覽器——這樣使用者不用自己開瀏覽器打網址。
static bool is_config_root_path(const char *req) {
    // 請求列格式："GET /path HTTP/1.1\r\n..."，只有整個路徑剛好是 "/" 才算根頁面。
    const char *path_start = strchr(req, ' ');
    if (path_start == NULL) {
        return false;
    }
    path_start++; // 跳過 "GET" 後的空格
    return path_start[0] == '/' && (path_start[1] == ' ' || path_start[1] == '?');
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

// 不用 strncasecmp（避免多依賴一個不確定 newlib-nano 有沒有帶的函式），手刻
// 一個大小寫不敏感比對，只用在這裡找 Content-Length 標頭。
static bool starts_with_ci(const char *s, const char *prefix) {
    for (; *prefix != '\0'; s++, prefix++) {
        char a = (*s >= 'A' && *s <= 'Z') ? (char)(*s - 'A' + 'a') : *s;
        char b = (*prefix >= 'A' && *prefix <= 'Z') ? (char)(*prefix - 'A' + 'a') : *prefix;
        if (a != b) {
            return false;
        }
    }
    return true;
}

// headers 收完（看到 \r\n\r\n）不代表 body 也收完了——TCP 常常把 headers 跟
// body 拆成不同封包送達，各自觸發一次 http_recv_cb。沒有這個檢查會在 body
// 還沒送到、甚至完全是空的那一刻就急著解析，永遠拿到空欄位（這是之前 ssid
// 一直是空字串的真正原因，不是使用者填錯或快取問題）。
static long parse_content_length(const char *req) {
    const char *p = req;
    const char key[] = "content-length:";
    size_t key_len = strlen(key);

    while ((p = strstr(p, "\n")) != NULL) {
        p++; // 移到這一行開頭
        if (starts_with_ci(p, key)) {
            return atol(p + key_len);
        }
    }
    return -1; // 沒有這個標頭（例如 GET 請求），或已經找到請求的結尾
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
            long content_length = parse_content_length(state->buf);
            size_t body_received = state->len - (size_t)(body - state->buf);
            if (content_length >= 0 && body_received < (size_t)content_length) {
                return ERR_OK; // body 還沒收完，繼續等下一段 TCP 資料
            }
            parse_and_store_form(body);
        }
        tcp_write(tpcb, SAVED_RESPONSE_HTML, sizeof(SAVED_RESPONSE_HTML) - 1, TCP_WRITE_FLAG_COPY);
    } else if (is_config_root_path(state->buf)) {
        tcp_write(tpcb, s_config_form_response, (u16_t)s_config_form_response_len, TCP_WRITE_FLAG_COPY);
    } else {
        // 不是根頁面：大概是作業系統的連通性檢測請求，導回根頁面觸發登入頁彈出。
        tcp_write(tpcb, REDIRECT_TO_ROOT_HTTP, sizeof(REDIRECT_TO_ROOT_HTTP) - 1, TCP_WRITE_FLAG_COPY);
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
    s_have_current_config = storage_load_config(&s_current_config);

    // 先掃描附近 WiFi、組好設定頁面 HTML，再開熱點——避免掃描期間熱點還沒
    // 準備好但已經廣播，使用者連上卻連不到頁面。
    scan_nearby_wifi();
    render_config_form_response();

    printf("[AP_CONFIG] starting hotspot ssid=\"%s\"\n", AP_SSID);
    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    display_status_show_ap_config(AP_SSID, AP_PASSWORD, s_have_current_config ? &s_current_config : NULL);

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
