#include "upload_api.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

// mbedtls_config_override.h 開了 MBEDTLS_PLATFORM_MS_TIME_ALT（BLE 配對用的
// mbedtls 需要這個平台掛鉤，跟這裡上傳走不走 TLS 無關，不能拿掉），因為預設
// 實作（platform_util.c）只支援 POSIX/Windows，bare-metal 編不過，這裡補上
// 要求的版本，包 pico SDK 的單調時鐘。
#include "mbedtls/platform_time.h"
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time());
}

#include "fora_protocol.h"
#include "led_status.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// 目前指向內網測試伺服器（後端團隊提供的 wgnursing API），純 HTTP、無 TLS——
// 見下面 upload_api_post_batch() 的說明。之後如果換成正式對外的 HTTPS 後端，
// 要把連線邏輯換回 altcp_tls（可以參考 git 歷史裡這個檔案先前的 TLS 版本），
// 並且把伺服器憑證填進 upload_tls_ca_cert.h（那個檔案先留著，這裡先不刪）。
// server_host/upload_server_host 可以透過 AP_CONFIG 表單設定，這裡的常數只是
// 沒設定過時的測試預設值。
#define UPLOAD_SERVER_HOST_DEFAULT "192.168.5.158"
#define UPLOAD_SERVER_PORT 8080
// location=yt 目前是後端測試環境給的固定值（範例 Postman request 裡就是這樣
// 帶），之後如果每個部署據點要帶不同的值，這裡要改成可設定的欄位，不能寫死。
#define UPLOAD_SERVER_PATH "/wgnursing/multi/division/v2/api/repository/uploadPhysiologicalData?location=yt"

// 後端要求 multipart/form-data，欄位名稱固定是 "data"，值是 JSON 字串（見
// build_request() 的說明）。這個 boundary 字串只要不會出現在我們自己組出來的
// JSON 內容裡就好，固定寫死一個夠獨特的字串即可，不需要每次隨機產生。
#define MULTIPART_BOUNDARY "PicoGatewayBoundary7MA4YWxkTrZu0gW"

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

// 把 1970-01-01 起算的天數換算成西曆年/月/日，Howard Hinnant 的
// civil_from_days（CC0 授權）——跟 display_status.c 的 civil_month_day_from_days()
// 是同一套演算法，那邊因為畫面空間有限只取月/日，這裡格式化人類可讀的日期
// 字串給後端用，需要完整的年份。
static void civil_from_days(int64_t z, int *year, unsigned *month, unsigned *day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);           // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    unsigned mp = (5 * doy + 2) / 153;                      // [0, 11]
    *day = doy - (153 * mp + 2) / 5 + 1;                    // [1, 31]
    *month = mp + (mp < 10 ? 3 : -9);                       // [1, 12]
    *year = (int)(y + (*month <= 2 ? 1 : 0));
}

// 後端的 uploadTime 欄位是 Java Date，之前送 epoch 毫秒數字時解析沒有出錯，
// 但伺服器記錄的日期跟預期差了一天——懷疑是後端把這個數字又轉了一次時區，
// 改成直接送人類可讀的本地時間字串（"yyyy-MM-dd HH:mm:ss"，台灣時間），
// 不含時區資訊，兩邊都不用再做任何時區換算，避免這種重複轉換的問題。
static void format_upload_time(uint64_t epoch_ms, char *out, size_t out_size) {
    uint64_t local_sec = epoch_ms / 1000 + LOCAL_UTC_OFFSET_SEC;
    int64_t days_since_epoch = (int64_t)(local_sec / 86400);
    uint32_t sec_of_day = (uint32_t)(local_sec % 86400);
    unsigned hour = sec_of_day / 3600;
    unsigned minute = (sec_of_day % 3600) / 60;
    unsigned second = sec_of_day % 60;

    int year;
    unsigned month, day;
    civil_from_days(days_since_epoch, &year, &month, &day);
    snprintf(out, out_size, "%04d-%02u-%02u %02u:%02u:%02u", year, month, day, hour, minute, second);
}

// 每種裝置種類各自的 dataSource 標籤——後端這個欄位是「上傳來源」，語意上
// 應該標示實際量測的醫材，不是我們的閘道器本身，所以要依裝置種類分開標示，
// 不能用同一個固定字串（見 upload_api_post_batch() 的說明，呼叫端已經依
// source_kind 分組，這裡只是把 kind 換成對應的字串）。10 字元內、不含空格。
static const char *data_source_label(uint8_t source_kind) {
    switch ((fora_device_kind_t)source_kind) {
        case FORA_DEVICE_THERMOMETER:   return "FORAIR42";
        case FORA_DEVICE_OXIMETER:      return "FORAO2";
        case FORA_DEVICE_BLOOD_PRESSURE: return "FORAD40";
        case FORA_DEVICE_MD6:           return "FORAMD6";
        // 不是 FORA 裝置，只是共用同一個編號空間，見 fora_protocol.h
        // FORA_DEVICE_RIGHTEST_GM700SB 的說明。
        case FORA_DEVICE_RIGHTEST_GM700SB: return "GM700SB";
        default:                        return "PicoGateway";
    }
}

// 後端一次上傳要的是「一筆彙整過的量測紀錄」（PhysioMeasurement，欄位見
// upload_api_post_batch() 的說明），不是像先前測試伺服器那樣的逐筆讀值陣列。
// 呼叫端已經保證傳進來的 records 都是同一個 source_kind（見
// upload_api_post_batch() 依裝置種類分組的說明），這裡不需要再處理混合來源
// 的情況。
static void build_measurement_json(char *json, size_t json_size, const char *patient_id,
                                    const vital_record_t *records, size_t count,
                                    uint64_t upload_time_ms, bool wall_clock_synced,
                                    uint8_t source_kind) {
    bool have_value[VITAL_TYPE_COUNT] = { false };
    float value[VITAL_TYPE_COUNT] = { 0 };
    uint8_t mode[VITAL_TYPE_COUNT] = { 0 };
    for (size_t i = 0; i < count; i++) {
        if (records[i].type < VITAL_TYPE_COUNT) {
            have_value[records[i].type] = true;
            value[records[i].type] = records[i].value;
            mode[records[i].type] = records[i].measurement_mode;
        }
    }

    size_t len = 0;
    append(json, json_size, &len, "{\"caseId\":\"");
    append_json_escaped(json, json_size, &len, patient_id);
    append(json, json_size, &len, "\",\"dataSource\":\"%s\"", data_source_label(source_kind));

    if (wall_clock_synced) {
        char upload_time_str[24];
        format_upload_time(upload_time_ms, upload_time_str, sizeof(upload_time_str));
        append(json, json_size, &len, ",\"uploadTime\":\"%s\"", upload_time_str);
    }

    // 後端血糖分空腹(glu_ac)/飯後(glu_pc)兩欄，2026-08-26 反編譯官方程式確認
    // byte[7] 高 2 bit 是量測情境（fora_measurement_mode_t），現在已經解析
    // 進 measurement_mode：PC（飯後）送 glu_pc，其餘（一般/飯前 AC，或沒有
    // 情境資訊的舊路徑)都送 glu_ac，跟後端原本「單一血糖欄位當空腹值」的
    // 既有慣例一致。
    if (have_value[VITAL_TYPE_GLUCOSE]) {
        const char *key = (mode[VITAL_TYPE_GLUCOSE] == FORA_MEASUREMENT_MODE_PC) ? "glu_pc" : "glu_ac";
        char value_str[16];
        format_value(value_str, sizeof(value_str), VITAL_TYPE_GLUCOSE, value[VITAL_TYPE_GLUCOSE]);
        append(json, json_size, &len, ",\"%s\":%s", key, value_str);
    }

    static const struct { vital_type_t type; const char *key; } FIELD_MAP[] = {
        { VITAL_TYPE_SYSTOLIC, "sbp" },
        { VITAL_TYPE_DIASTOLIC, "dbp" },
        { VITAL_TYPE_PULSE_RATE, "bpm" },
        { VITAL_TYPE_TEMPERATURE, "temp" },
        { VITAL_TYPE_SPO2, "spo2" },
        // MD6 六合一測試儀血糖以外的 5 項——2026-08-26 只有實機驗證過血糖/HCT
        // 不用縮放，Ketone/UA/CHOL 這 3 項先套用同樣的假設（raw value 直接
        // 送出），還沒逐項驗證，見 PROJECT_PLAN.md 第 6.5 節。欄位名稱是後端
        // 目前還沒有的新欄位，Gson 解析時會靜默忽略、不會報錯也不會存進
        // 資料庫，先把格式送過去，等後端加上對應欄位。
        { VITAL_TYPE_HCT, "hct" },
        { VITAL_TYPE_KETONE, "ketone" },
        { VITAL_TYPE_UA, "ua" },
        { VITAL_TYPE_CHOL, "chol" },
        // hb 這個欄位的值不是裝置量到的——MD6 沒有獨立的 Hb 記錄，這是
        // fora_protocol.c 用 HCT×0.34 換算出來的估計值，只用 2 個實機樣本
        // 驗證過換算公式，見該檔案跟 PROJECT_PLAN.md 第 6.5 節的說明。
        { VITAL_TYPE_HB, "hb" },
    };
    for (size_t f = 0; f < sizeof(FIELD_MAP) / sizeof(FIELD_MAP[0]); f++) {
        vital_type_t type = FIELD_MAP[f].type;
        if (!have_value[type]) {
            continue;
        }
        char value_str[16];
        format_value(value_str, sizeof(value_str), type, value[type]);
        append(json, json_size, &len, ",\"%s\":%s", FIELD_MAP[f].key, value_str);
    }
    // resp_rate（呼吸率）、weight（體重）：目前沒有任何裝置量測這兩項，
    // 照後端說的「沒有值就不用填」，整個省略這兩個 key。
    append(json, json_size, &len, "}");
}

static size_t build_request(const char *patient_id, const char *server_host, const char *api_key,
                             const vital_record_t *records, size_t count,
                             uint64_t upload_time_ms, bool wall_clock_synced, uint8_t source_kind) {
    char json[512];
    build_measurement_json(json, sizeof(json), patient_id, records, count, upload_time_ms, wall_clock_synced,
                            source_kind);

    // 後端要求 multipart/form-data，單一欄位 "data" 帶上面組好的 JSON 字串
    // （見 upload_api_post_batch() 開頭的說明），不是直接把 JSON 當 body。
    char body[REQUEST_BUF_SIZE - 256];
    size_t body_len = 0;
    append(body, sizeof(body), &body_len, "--%s\r\n", MULTIPART_BOUNDARY);
    append(body, sizeof(body), &body_len, "Content-Disposition: form-data; name=\"data\"\r\n\r\n");
    append(body, sizeof(body), &body_len, "%s", json);
    append(body, sizeof(body), &body_len, "\r\n--%s--\r\n", MULTIPART_BOUNDARY);

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

    // Host 標頭帶上 port——目標是非預設埠(8080)，省略 port 在某些伺服器/代理
    // 上可能無法正確路由。
    int n = snprintf(s_request, sizeof(s_request),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s:%u\r\n"
                      "Content-Type: multipart/form-data; boundary=%s\r\n"
                      "Content-Length: %u\r\n"
                      "%s"
                      "Connection: close\r\n\r\n"
                      "%s",
                      UPLOAD_SERVER_PATH, server_host, (unsigned)UPLOAD_SERVER_PORT, MULTIPART_BOUNDARY,
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
    printf("[UPLOAD_API] connection error (err=%d)\n", err);
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

// 目前對接的是後端團隊給的內網測試環境（wgnursing API），純 HTTP、無 TLS——
// 走的是院內區網，不是公開網際網路，這一版先求能跟真正的後端 API 格式打通。
// 之後如果要換成正式對外部署的 HTTPS 後端，這個函式要改回用 altcp_tls（可以
// 參考 git 歷史裡這個檔案先前的版本），並把伺服器憑證填進 upload_tls_ca_cert.h。
//
// records 必須全部是同一個 source_kind——呼叫端（mode_upload.c）負責先把
// 待傳批次依裝置種類分組，每組各自呼叫一次這個函式，才能讓 dataSource 正確
// 標示是哪種醫材量的（見 data_source_label() 的說明），不能在這裡混合不同
// 來源一起送，後端一筆 PhysioMeasurement 只有一個 dataSource 欄位。
bool upload_api_post_batch(const char *patient_id, const char *server_host, const char *api_key,
                           const vital_record_t *records, size_t count,
                           uint64_t upload_time_ms, bool wall_clock_synced, uint8_t source_kind) {
    if (count == 0) {
        return true;
    }

    const char *effective_host =
        (server_host != NULL && server_host[0] != '\0') ? server_host : UPLOAD_SERVER_HOST_DEFAULT;

    build_request(patient_id, effective_host, api_key, records, count, upload_time_ms, wall_clock_synced,
                   source_kind);
    // 排查過伺服器回 400 Bad Request（代表 JSON 解析失敗）的問題，印出實際
    // 組出來的請求，方便直接比對是不是 JSON 格式本身有問題（例如 patient_id
    // 裡含有沒跳脫的雙引號／反斜線——AP_CONFIG 表單的 patient_id 是自由輸入
    // 欄位，build_request() 目前組 JSON 時沒有對它做任何跳脫）。**注意**：
    // 這行會把認證金鑰明文印進序列埠 log，只在開發除錯階段這樣做，正式環境
    // 若序列埠 log 會被留存/上傳，要拿掉這行或遮蔽金鑰。
    printf("[UPLOAD_API] request:\n%s\n", s_request);

    // 1. 先解析伺服器主機名的 IP。目前的測試目標本身就是純 IP（不是網域名），
    //    dns_gethostbyname() 對純 IP 字串一樣能用（lwIP 會直接辨識、不用真的
    //    查詢），之後如果 server_host 換成網域名也不用改這裡的邏輯。
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

    // 2. 建立純 TCP 連線（見函式開頭的說明，內網測試環境沒有 TLS）。
    upload_ctx_t ctx = { .pcb = NULL, .done = false, .success = false };

    cyw43_arch_lwip_begin();
    struct altcp_pcb *pcb = altcp_tcp_new();
    if (pcb == NULL) {
        cyw43_arch_lwip_end();
        printf("[UPLOAD_API] altcp_tcp_new failed\n");
        return false;
    }
    ctx.pcb = pcb;

    altcp_arg(pcb, &ctx);
    altcp_recv(pcb, on_recv);
    altcp_err(pcb, on_err);

    err_t connect_err = altcp_connect(pcb, &dns_ctx.addr, UPLOAD_SERVER_PORT, on_connected);
    cyw43_arch_lwip_end();

    if (connect_err != ERR_OK) {
        printf("[UPLOAD_API] altcp_connect failed (err=%d)\n", connect_err);
        return false;
    }

    printf("[UPLOAD_API] POST %u pending record(s) to http://%s:%u%s\n",
           (unsigned)count, effective_host, (unsigned)UPLOAD_SERVER_PORT, UPLOAD_SERVER_PATH);

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
        return false;
    }

    printf("[UPLOAD_API] result: %s\n", ctx.success ? "success" : "failed");
    if (!ctx.success) {
        // 印出實際收到的回應開頭，方便排查是判斷邏輯沒抓到 200，還是伺服器
        // 真的回了別的狀態碼/完全沒回應。
        printf("[UPLOAD_API]   response so far (%u bytes): %s\n",
               (unsigned)ctx.header_len, ctx.header_len > 0 ? ctx.header_buf : "(none)");
    }
    return ctx.success;
}
