#include "upload_api.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

// 只有在 pico_lwip_mbedtls 有把 lib/lwip/src/apps/altcp_tls 加進 include path 時
// 才拿得到（mbedtls 3.x 才會加，見 pico-sdk 的 pico_lwip/CMakeLists.txt）。
// 拿這個 struct 是為了取出裡面的 mbedtls_ssl_context 手動設定 SNI hostname——
// lwIP 的 altcp_tls 封裝本身沒有提供設定 hostname 的介面。
#include "altcp_tls_mbedtls_structs.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"

#include "led_status.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// mbedtls_config_override.h 開了 MBEDTLS_PLATFORM_MS_TIME_ALT，因為預設實作
// （platform_util.c）只支援 POSIX/Windows，bare-metal 編不過，這裡補上要求的
// 版本，包 pico SDK 的單調時鐘（開機以來的毫秒數，TLS 只拿來算相對時間，不需要
// 真實日曆時間）。
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time());
}

// TODO: 正式版要讓伺服器位址可透過 AP 設定頁面輸入；device_config_t 目前沒有
// 這個欄位，測試階段先寫死方便驗證「BLE 收到 -> WiFi 上傳 -> 網站看得到」整條路
// 能不能打通，且不假設 Pico 部署現場的 WiFi 能連到跟開發機同一個區網，直接走
// 公開網際網路（Cloudflare Tunnel）。之後要接正式後端時，這裡要換成真實 endpoint、
// 認證方式，且 tunnel 網址是暫時的，每次重啟 cloudflared 都會換一個新的。
#define UPLOAD_SERVER_HOST "clip-perth-individual-andale.trycloudflare.com"
#define UPLOAD_SERVER_PORT 443
#define UPLOAD_SERVER_PATH "/api/vitals"

#define DNS_TIMEOUT_MS       8000
#define UPLOAD_TIMEOUT_MS    15000
#define REQUEST_BUF_SIZE     4096
#define RESPONSE_PEEK_SIZE   64

typedef struct {
    ip_addr_t addr;
    volatile bool done;
    volatile bool ok;
} dns_ctx_t;

typedef struct {
    struct altcp_pcb *pcb;
    volatile bool done;
    volatile bool success;
} upload_ctx_t;

static char s_request[REQUEST_BUF_SIZE];

// 邊界安全的字串附加：buf_size 內一定會留一個 '\0' 的位置，超過的部分直接捨棄。
static void append(char *buf, size_t buf_size, size_t *len, const char *fmt, ...) {
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

// 手動格式化數值，避免依賴 newlib-nano 預設未啟用的 printf float 支援
// （沿用 mode_ble_receive.c 同樣的作法）。體溫只需要小數點後 1 位（裝置本身
// 解析度就是 0.1°C），其他類型（SpO2、脈搏、血壓）都是整數。用四捨五入而不是
// 無條件捨去——之前用 (int)((value-whole)*100) 這種截斷法，浮點數誤差會讓
// 36.8 被印成 36.79 這種看起來莫名其妙的錯誤數字。
static void format_value(char *out, size_t out_size, vital_type_t type, float value) {
    if (type == VITAL_TYPE_TEMPERATURE) {
        int tenths = (int)(value * 10.0f + (value >= 0.0f ? 0.5f : -0.5f));
        int whole = tenths / 10;
        int frac = tenths % 10;
        if (frac < 0) {
            frac = -frac;
        }
        snprintf(out, out_size, "%d.%d", whole, frac);
    } else {
        int rounded = (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
        snprintf(out, out_size, "%d", rounded);
    }
}

static size_t build_request(const char *patient_id, const vital_record_t *records, size_t count) {
    char body[REQUEST_BUF_SIZE - 256];
    size_t body_len = 0;

    append(body, sizeof(body), &body_len, "{\"patient_id\":\"%s\",\"readings\":[",
           patient_id ? patient_id : "");

    for (size_t i = 0; i < count; i++) {
        char value_str[16];
        format_value(value_str, sizeof(value_str), records[i].type, records[i].value);
        append(body, sizeof(body), &body_len,
               "%s{\"type\":%d,\"value\":%s,\"received_at_ms\":%llu}",
               i == 0 ? "" : ",", (int)records[i].type, value_str,
               (unsigned long long)records[i].received_at_ms);
    }
    append(body, sizeof(body), &body_len, "]}");

    int n = snprintf(s_request, sizeof(s_request),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n\r\n"
                      "%s",
                      UPLOAD_SERVER_PATH, UPLOAD_SERVER_HOST,
                      (unsigned)body_len, body);
    return n > 0 ? (size_t)n : 0;
}

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    dns_ctx_t *ctx = (dns_ctx_t *)arg;
    if (ipaddr != NULL) {
        ctx->addr = *ipaddr;
        ctx->ok = true;
    }
    ctx->done = true;
}

static err_t on_recv(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err) {
    (void)err;
    upload_ctx_t *ctx = (upload_ctx_t *)arg;

    if (p == NULL) {
        // 對方關閉連線，代表回應已經收完。
        altcp_close(conn);
        ctx->done = true;
        return ERR_OK;
    }

    char peek[RESPONSE_PEEK_SIZE];
    uint16_t copy_len = p->tot_len < sizeof(peek) - 1 ? p->tot_len : sizeof(peek) - 1;
    pbuf_copy_partial(p, peek, copy_len, 0);
    peek[copy_len] = '\0';

    if (strstr(peek, "200") != NULL) {
        ctx->success = true;
    }

    altcp_recved(conn, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    upload_ctx_t *ctx = (upload_ctx_t *)arg;
    printf("[UPLOAD_API] tls connection error (err=%d)\n", err);
    // lwIP 呼叫這個 callback 之後 pcb 已經被釋放，不能再對它做任何操作。
    ctx->success = false;
    ctx->done = true;
}

static err_t on_connected(void *arg, struct altcp_pcb *conn, err_t err) {
    upload_ctx_t *ctx = (upload_ctx_t *)arg;
    if (err != ERR_OK) {
        printf("[UPLOAD_API] connect callback err=%d\n", err);
        ctx->done = true;
        return err;
    }

    size_t request_len = strlen(s_request);
    err_t write_err = altcp_write(conn, s_request, (u16_t)request_len, TCP_WRITE_FLAG_COPY);
    if (write_err != ERR_OK) {
        printf("[UPLOAD_API] altcp_write failed (err=%d)\n", write_err);
        ctx->done = true;
        return write_err;
    }
    altcp_output(conn);
    return ERR_OK;
}

bool upload_api_post_batch(const char *patient_id, const vital_record_t *records, size_t count) {
    if (count == 0) {
        return true;
    }

    build_request(patient_id, records, count);

    // 1. 先解析 UPLOAD_SERVER_HOST 的 IP（tunnel 位址不是固定 IP，且 altcp_connect
    //    只接受 IP，SNI hostname 另外用 mbedtls_ssl_set_hostname() 設定）。
    dns_ctx_t dns_ctx = { .done = false, .ok = false };
    cyw43_arch_lwip_begin();
    err_t dns_err = dns_gethostbyname(UPLOAD_SERVER_HOST, &dns_ctx.addr, dns_found_cb, &dns_ctx);
    cyw43_arch_lwip_end();

    if (dns_err == ERR_OK) {
        dns_ctx.ok = true;
        dns_ctx.done = true;
    } else if (dns_err != ERR_INPROGRESS) {
        printf("[UPLOAD_API] dns_gethostbyname failed immediately (err=%d)\n", dns_err);
        return false;
    } else {
        absolute_time_t dns_deadline = make_timeout_time_ms(DNS_TIMEOUT_MS);
        while (!dns_ctx.done && !time_reached(dns_deadline)) {
            sleep_ms(20);
        }
    }

    if (!dns_ctx.ok) {
        printf("[UPLOAD_API] DNS resolve of \"%s\" failed or timed out\n", UPLOAD_SERVER_HOST);
        return false;
    }

    // 2. 建立 TLS 連線。沒有內嵌 CA 憑證，預設 authmode 是 MBEDTLS_SSL_VERIFY_OPTIONAL，
    //    握手會照常完成但不驗證憑證鏈——測試階段先求連得通，正式部署前要內嵌信任的
    //    CA 憑證並改成要求驗證，否則有中間人攻擊風險。
    struct altcp_tls_config *tls_config = altcp_tls_create_config_client(NULL, 0);
    if (tls_config == NULL) {
        printf("[UPLOAD_API] altcp_tls_create_config_client failed\n");
        return false;
    }

    upload_ctx_t ctx = { .pcb = NULL, .done = false, .success = false };

    cyw43_arch_lwip_begin();
    struct altcp_pcb *pcb = altcp_tls_new(tls_config, IPADDR_TYPE_V4);
    if (pcb == NULL) {
        cyw43_arch_lwip_end();
        printf("[UPLOAD_API] altcp_tls_new failed\n");
        altcp_tls_free_config(tls_config);
        return false;
    }
    ctx.pcb = pcb;

    // SNI：把目標主機名告訴 TLS 層，Cloudflare 這類多租戶邊緣節點靠這個決定要
    // 把連線轉給哪個 tunnel，沒設定的話交握可能失敗或連到錯的後端。
    altcp_mbedtls_state_t *tls_state = (altcp_mbedtls_state_t *)altcp_tls_context(pcb);
    mbedtls_ssl_set_hostname(&tls_state->ssl_context, UPLOAD_SERVER_HOST);

    altcp_arg(pcb, &ctx);
    altcp_recv(pcb, on_recv);
    altcp_err(pcb, on_err);

    err_t connect_err = altcp_connect(pcb, &dns_ctx.addr, UPLOAD_SERVER_PORT, on_connected);
    cyw43_arch_lwip_end();

    if (connect_err != ERR_OK) {
        printf("[UPLOAD_API] altcp_connect failed (err=%d)\n", connect_err);
        altcp_tls_free_config(tls_config);
        return false;
    }

    printf("[UPLOAD_API] POST %u reading(s) to https://%s%s\n",
           (unsigned)count, UPLOAD_SERVER_HOST, UPLOAD_SERVER_PATH);

    absolute_time_t deadline = make_timeout_time_ms(UPLOAD_TIMEOUT_MS);
    while (!ctx.done && !time_reached(deadline)) {
        led_status_poll();
        sleep_ms(20);
    }

    if (!ctx.done) {
        printf("[UPLOAD_API] timed out waiting for response\n");
        cyw43_arch_lwip_begin();
        altcp_abort(ctx.pcb);
        cyw43_arch_lwip_end();
        altcp_tls_free_config(tls_config);
        return false;
    }

    altcp_tls_free_config(tls_config);
    printf("[UPLOAD_API] result: %s\n", ctx.success ? "success" : "failed");
    return ctx.success;
}
