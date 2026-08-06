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
#include "upload_tls_ca_cert.h"

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

// 伺服器位址現在可以透過 AP_CONFIG 表單設定（device_config_t.upload_server_host，
// 見 upload_api_post_batch() 的 server_host 參數），這裡的常數只是「沒設定過
// 時」的測試預設值，方便開發階段驗證「BLE 收到 -> WiFi 上傳 -> 網站看得到」
// 整條路能不能打通，不用先跑一次 AP_CONFIG。目前指向 Cloudflare Tunnel 的
// 臨時測試網址，重開 `cloudflared` 就會換掉，不是給正式環境用的。
#define UPLOAD_SERVER_HOST_DEFAULT "clip-perth-individual-andale.trycloudflare.com"
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
    // 累積收到的回應開頭幾個 byte，跨越多次 on_recv() 呼叫——回應可能被
    // TLS record／pbuf 邊界切成好幾段送達，狀態列 "HTTP/1.1 200 OK" 可能被
    // 切在中間，只看單次收到的內容會找不到完整的 "200"。
    char header_buf[128];
    size_t header_len;
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

// 把字串當成 JSON 字串內容附加進 buf（外層的引號不包含在內，呼叫端自己包）。
// patient_id 是 AP_CONFIG 表單的自由輸入欄位，內容完全不受控制，可能包含
// 控制字元，必須跳脫過後才能塞進 JSON。
static void append_json_escaped(char *buf, size_t buf_size, size_t *len, const char *text) {
    if (text == NULL) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            append(buf, buf_size, len, "\\%c", *p);
        } else if (*p < 0x20) {
            append(buf, buf_size, len, "\\u%04x", *p);
        } else {
            append(buf, buf_size, len, "%c", *p);
        }
    }
}

// 手動格式化數值，避免依賴 newlib-nano 預設未啟用的 printf float 支援
// （沿用 mode_ble_receive.c 同樣的作法）。體溫只需要小數點後 1 位（裝置本身
// 解析度就是 0.1°C），其他類型（SpO2、脈搏、血壓）都是整數。用四捨五入到
// 目標精度再格式化，避免浮點數截斷誤差。
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

static size_t build_request(const char *patient_id, const char *server_host, const char *api_key,
                             const vital_record_t *records, size_t count) {
    char body[REQUEST_BUF_SIZE - 256];
    size_t body_len = 0;

    append(body, sizeof(body), &body_len, "{\"patient_id\":\"");
    append_json_escaped(body, sizeof(body), &body_len, patient_id);
    append(body, sizeof(body), &body_len, "\",\"readings\":[");

    for (size_t i = 0; i < count; i++) {
        char value_str[16];
        format_value(value_str, sizeof(value_str), records[i].type, records[i].value);
        append(body, sizeof(body), &body_len,
               "%s{\"type\":%d,\"value\":%s,\"received_at_ms\":%llu}",
               i == 0 ? "" : ",", (int)records[i].type, value_str,
               (unsigned long long)records[i].received_at_ms);
    }
    append(body, sizeof(body), &body_len, "]}");

    // 認證金鑰是 AP_CONFIG 的自由輸入欄位，跟 patient_id 一樣不受韌體控制，
    // 理論上可能包含 CRLF 之類會破壞 HTTP 標頭格式的字元；這裡簡單起見只
    // 允許可印出 ASCII、遇到會弄亂標頭格式的字元就整段跳過不送出這個標頭，
    // 比送出格式錯誤的請求安全（伺服器端收不到認證標頭會直接拒絕，不會
    // 誤判成通過）。
    char auth_header[UPLOAD_API_KEY_MAX_LEN + 32] = "";
    if (api_key != NULL && api_key[0] != '\0') {
        bool key_is_safe = true;
        for (const char *p = api_key; *p != '\0'; p++) {
            if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) {
                key_is_safe = false;
                break;
            }
        }
        if (key_is_safe) {
            snprintf(auth_header, sizeof(auth_header), "X-API-Key: %s\r\n", api_key);
        }
    }

    int n = snprintf(s_request, sizeof(s_request),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %u\r\n"
                      "%s"
                      "Connection: close\r\n\r\n"
                      "%s",
                      UPLOAD_SERVER_PATH, server_host,
                      (unsigned)body_len, auth_header, body);
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

// 檢查累積的回應開頭是不是 HTTP 狀態列回報 200（例如 "HTTP/1.1 200 OK"），
// 只比對狀態碼欄位本身，不是在整段回應內容裡找 "200" 子字串（標頭/內文其他
// 地方，例如 Content-Length 剛好是 200，不該被誤判成成功）。
static bool header_indicates_200(const char *buf) {
    const char *sp = strchr(buf, ' ');
    if (sp == NULL) {
        return false;
    }
    const char *code = sp + 1;
    if (strncmp(code, "200", 3) != 0) {
        return false;
    }
    char after = code[3];
    return after == ' ' || after == '\r' || after == '\0';
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

    // 累積進 header_buf（不是每次只看這次收到的片段），這樣狀態列剛好被
    // TCP/TLS 切成兩段送達也不會漏判，見 upload_ctx_t 裡的說明。
    if (!ctx->success && ctx->header_len + 1 < sizeof(ctx->header_buf)) {
        size_t space = sizeof(ctx->header_buf) - 1 - ctx->header_len;
        uint16_t copy_len = p->tot_len < space ? p->tot_len : (uint16_t)space;
        pbuf_copy_partial(p, ctx->header_buf + ctx->header_len, copy_len, 0);
        ctx->header_len += copy_len;
        ctx->header_buf[ctx->header_len] = '\0';

        if (header_indicates_200(ctx->header_buf)) {
            ctx->success = true;
        }
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

bool upload_api_post_batch(const char *patient_id, const char *server_host, const char *api_key,
                           const vital_record_t *records, size_t count) {
    if (count == 0) {
        return true;
    }

    const char *effective_host =
        (server_host != NULL && server_host[0] != '\0') ? server_host : UPLOAD_SERVER_HOST_DEFAULT;

    build_request(patient_id, effective_host, api_key, records, count);
    // 排查過伺服器回 400 Bad Request（代表 JSON 解析失敗）的問題，印出實際
    // 組出來的請求，方便直接比對是不是 JSON 格式本身有問題（例如 patient_id
    // 裡含有沒跳脫的雙引號／反斜線——AP_CONFIG 表單的 patient_id 是自由輸入
    // 欄位，build_request() 目前組 JSON 時沒有對它做任何跳脫）。**注意**：
    // 這行會把認證金鑰明文印進序列埠 log，只在開發除錯階段這樣做，正式環境
    // 若序列埠 log 會被留存/上傳，要拿掉這行或遮蔽金鑰。
    printf("[UPLOAD_API] request:\n%s\n", s_request);

    // 1. 先解析伺服器主機名的 IP（tunnel 位址不是固定 IP，且 altcp_connect
    //    只接受 IP，SNI hostname 另外用 mbedtls_ssl_set_hostname() 設定）。
    dns_ctx_t dns_ctx = { .done = false, .ok = false };
    cyw43_arch_lwip_begin();
    err_t dns_err = dns_gethostbyname(effective_host, &dns_ctx.addr, dns_found_cb, &dns_ctx);
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
        printf("[UPLOAD_API] DNS resolve of \"%s\" failed or timed out\n", effective_host);
        return false;
    }

    // 2. 建立 TLS 連線。有沒有驗證伺服器憑證鏈完全取決於 UPLOAD_CA_CERT_PEM
    //    有沒有內容（見 upload_tls_ca_cert.h 的說明）：lwIP 的 altcp_tls 封裝
    //    收到非空的 CA 內容會自動把驗證模式設成 MBEDTLS_SSL_VERIFY_REQUIRED，
    //    沒有的話退回 MBEDTLS_SSL_VERIFY_OPTIONAL（等同不驗證，握手照常完成，
    //    中間人偽造的憑證也會被接受）——目前還沒有正式後端網址可以嵌入真正
    //    的憑證，所以是空字串，只適合開發測試階段，見 upload_tls_ca_cert.h。
    size_t ca_cert_len = sizeof(UPLOAD_CA_CERT_PEM) > 1 ? sizeof(UPLOAD_CA_CERT_PEM) : 0;
    if (ca_cert_len == 0) {
        static bool s_warned_no_verify = false;
        if (!s_warned_no_verify) {
            printf("[UPLOAD_API] WARNING: no CA certificate embedded (see upload_tls_ca_cert.h) - "
                   "TLS connections are NOT verifying the server certificate, vulnerable to "
                   "man-in-the-middle. Do not deploy like this in production.\n");
            s_warned_no_verify = true;
        }
    }
    struct altcp_tls_config *tls_config = altcp_tls_create_config_client(
        ca_cert_len > 0 ? (const uint8_t *)UPLOAD_CA_CERT_PEM : NULL, ca_cert_len);
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
    mbedtls_ssl_set_hostname(&tls_state->ssl_context, effective_host);

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
           (unsigned)count, effective_host, UPLOAD_SERVER_PATH);

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
    if (!ctx.success) {
        // 印出實際收到的回應開頭，方便排查是判斷邏輯沒抓到 200，還是伺服器
        // 真的回了別的狀態碼/完全沒回應。
        printf("[UPLOAD_API]   response so far (%u bytes): %s\n",
               (unsigned)ctx.header_len, ctx.header_len > 0 ? ctx.header_buf : "(none)");
    }
    return ctx.success;
}
