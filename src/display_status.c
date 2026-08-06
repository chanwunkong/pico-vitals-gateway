#include "display_status.h"

#include "DEV_Config.h"
#include "EPD_2in9_V2.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "fora_protocol.h"
#include "qrcodegen.h"
#include "storage.h"
#include "wall_clock.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

// EPD_2IN9_V2_WIDTH/HEIGHT 是面板原生方向（直式 128x296）；畫面用 Rotate=90
// 轉成橫式（296x128）比較好排版，framebuffer 大小照原生方向算：
// (128/8) * 296 = 4736 bytes。用靜態陣列，不用 malloc（跟專案其他地方一致）。
static uint8_t s_framebuffer[(EPD_2IN9_V2_WIDTH / 8) * EPD_2IN9_V2_HEIGHT];

// ---------------------------------------------------------------------------
// 共用小工具
// ---------------------------------------------------------------------------

// 把可能含非可印出 ASCII byte（例如 UTF-8 多位元組中文字的每個 byte）的字串，
// 過濾成只剩 0x20-0x7E 的可印出 ASCII 字元。用在畫面顯示使用者自由輸入的欄位
// （例如 patient_id）之前——EPD 字型只支援 ASCII，GUI_Paint.c 的
// Paint_DrawChar() 用 (char - ' ') 直接當 flash 位址偏移量，收到超出字型表
// 範圍的 byte 可能亂碼甚至讀到無效位址，見 PROJECT_PLAN.md 12.3 節。
static void sanitize_ascii(const char *src, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    size_t n = 0;
    for (const char *p = src; *p != '\0' && n < out_size - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E) {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
}

// 手動格式化數值，避免依賴 newlib-nano 預設未啟用的 printf float 支援，
// 沿用 upload_api.c 的 format_value()／mode_ble_receive.c 同樣的作法：
// 體溫取到小數點後 1 位、其他類型都是整數，四捨五入而非無條件捨去
// （截斷法會讓 36.8 這種值因浮點數誤差被印成 36.79，見 PROJECT_PLAN.md 第 8
// 節第 10 點）。
static void format_vital_value(char *out, size_t out_size, vital_type_t type, float value) {
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

// 使用者反應「幾分鐘前」這種相對時間不好用，改成顯示實際時鐘時間（台灣
// UTC+8）。只有 wall_clock 這次開機以來至少成功校時過一次才有真實時間可以
// 換算——校時成功之後，任何 boot-relative 時間點（不管是校時之前還是之後
// 量到的）都能正確換算，見 wall_clock_to_epoch_ms() 的說明。從沒校時成功過
// 的話沒有真實時間基準，退回顯示開機經過分鐘數並註明「未校時」，不要假裝
// 是準確的時鐘時間。時區假設（UTC+8）跟 fora_protocol.c 共用，見 common.h 的
// LOCAL_UTC_OFFSET_SEC。

// 把 1970-01-01 起算的天數換算成西曆月/日，公開的整數演算法（Howard Hinnant
// 的 civil_from_days，CC0 授權，各種語言的標準函式庫/曆法程式常見的實作），
// 不需要拉 <time.h>/mktime() 那一整套進來，純整數運算，embedded 環境很好用。
// 年份沒算（畫面空間有限，且個案不太可能看著跨年的舊資料），只取月/日。
static void civil_month_day_from_days(int64_t days_since_epoch, unsigned *month, unsigned *day) {
    int64_t z = days_since_epoch + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);           // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    unsigned mp = (5 * doy + 2) / 153;                      // [0, 11]
    *day = doy - (153 * mp + 2) / 5 + 1;                    // [1, 31]
    *month = mp + (mp < 10 ? 3 : -9);                       // [1, 12]
}

void display_status_format_clock(uint64_t boot_ms, char *out, size_t out_size) {
    if (!wall_clock_is_synced()) {
        uint64_t now_ms = to_ms_since_boot(get_absolute_time());
        uint64_t age_min = (now_ms >= boot_ms ? (now_ms - boot_ms) : 0) / 60000;
        snprintf(out, out_size, "unsynced,+%um", (unsigned)age_min);
        return;
    }
    uint64_t epoch_ms = wall_clock_to_epoch_ms(boot_ms);
    uint64_t local_sec = epoch_ms / 1000 + LOCAL_UTC_OFFSET_SEC;
    int64_t days_since_epoch = (int64_t)(local_sec / 86400);
    uint32_t sec_of_day = (uint32_t)(local_sec % 86400);
    unsigned hour = sec_of_day / 3600;
    unsigned minute = (sec_of_day % 3600) / 60;

    unsigned month, day;
    civil_month_day_from_days(days_since_epoch, &month, &day);
    snprintf(out, out_size, "%02u/%02u %02u:%02u", month, day, hour, minute);
}

static void begin_frame(void) {
    Paint_SelectImage(s_framebuffer);
    Paint_Clear(WHITE);
}

// ---------------------------------------------------------------------------
// 面板保護：不刷新的時候要進睡眠或斷電，每次刷新都會遵守。刷新頻率不設下限，
// 只保留「至少每 24 小時要刷新一次」避免長時間靜態顯示造成殘影/老化。
// ---------------------------------------------------------------------------
#define MAX_REFRESH_INTERVAL_MS (24ULL * 60 * 60 * 1000) // 24 小時至少刷新一次

static bool s_have_refreshed_once = false;
static absolute_time_t s_last_actual_refresh;

// 目前面板上顯示的是不是 BLE_RECEIVE 畫面。AP_CONFIG／UPLOAD／錯誤／開機驗證
// 畫面都是「直接畫、直接刷新」，不會去更新 BLE_RECEIVE 那邊「上次真的畫了
// 什麼」的快照（`s_last_rendered`，見下面 BLE_RECEIVE 那一段）。這個旗標
// 強制：只要中間顯示過別的畫面，回到 BLE_RECEIVE 時第一次 poll() 一定要
// 刷新一次，不管內容比對結果如何。
static bool s_ble_screen_is_current = false;

// 回傳值目前恆為 true（沒有任何情況會跳過刷新），保留 bool 是因為
// display_status_poll() 的「沒刷新成功就不更新快照」邏輯還在，之後如果又要
// 加別的跳過條件（例如偵測到面板故障）不用改呼叫端。只用全刷
// （`EPD_2IN9_V2_Display_Base`），不做局部刷新。
static bool end_frame_and_refresh(void) {
    // EPD_2IN9_V2_Init() 內部一開始就會做硬體 Reset，同時也是从深度睡眠喚醒的
    // 標準程序，所以不需要另外維護一個「現在是睡是醒」的狀態——每次要刷新
    // 畫面前都直接呼叫一次就對了。
    EPD_2IN9_V2_Init();
    EPD_2IN9_V2_Display_Base(s_framebuffer);
    // 刷完立刻進深度睡眠，面板不能一直維持高電壓，見上面的說明。
    EPD_2IN9_V2_Sleep();

    s_last_actual_refresh = get_absolute_time();
    s_have_refreshed_once = true;
    return true;
}

// ---------------------------------------------------------------------------
// 初始化 / 開機驗證畫面
// ---------------------------------------------------------------------------

void display_status_init(void) {
    if (DEV_Module_Init() != 0) {
        printf("[DISPLAY] DEV_Module_Init() failed\n");
        return;
    }

    // 注意：這裡故意不呼叫 EPD_2IN9_V2_Init()/Clear()——那本身就是一次面板
    // 刷新，而開機後幾乎立刻就會呼叫某個 display_status_show_xxx() 畫出真正
    // 的第一個畫面，兩次刷新背靠背做沒有意義。面板真正的硬體初始化延後到
    // 第一次呼叫 display_status_show_xxx()／poll() 觸發刷新時才做。
    Paint_NewImage(s_framebuffer, EPD_2IN9_V2_WIDTH, EPD_2IN9_V2_HEIGHT, ROTATE_90, WHITE);
    begin_frame();
}

void display_status_show_boot_test(void) {
    s_ble_screen_is_current = false;
    begin_frame();

    Paint_DrawString_EN(10, 10, "Pico Vitals Gateway", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(10, 40, "e-Paper OK", &Font12, BLACK, WHITE);
    Paint_DrawRectangle(5, 5, 290, 122, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

    end_frame_and_refresh();
    printf("[DISPLAY] boot test screen shown\n");
}

// ---------------------------------------------------------------------------
// AP_CONFIG / UPLOAD / 錯誤畫面：內容在該模式執行期間變動不頻繁，直接畫、
// 直接刷新，呼叫端自己控制呼叫時機，不需要額外的非阻塞排程。
// ---------------------------------------------------------------------------

// 手機相機掃到 "WIFI:...;;" 這個特定前綴的字串會自動跳出「加入 WiFi」的系統
// 提示，這是業界慣例（源自 ZXing），不是 QR code 本身有什麼特殊格式/模式——
// 產生方式跟其他 QR code 完全一樣，差別只在編碼進去的文字內容。SSID/密碼裡
// 如果剛好出現 `\`、`;`、`,`、`:`、`"` 這幾個字元，依慣例要加反斜線跳脫，
// 不然會被手機誤判成欄位分隔符號——`mode_ap_config.c` 的 `generate_ap_ssid()`／
// `generate_ap_password()` 衍生出來的字串目前不會出現這些字元，但這裡還是
// 做完整，避免兩邊之後改了衍生規則卻忘記回來檢查這裡。
static void append_escaped_wifi_field(char *out, size_t out_size, size_t *len, const char *field) {
    for (const char *p = field; *p != '\0' && *len + 1 < out_size; p++) {
        if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"') {
            if (*len + 2 >= out_size) {
                break;
            }
            out[(*len)++] = '\\';
        }
        out[(*len)++] = *p;
    }
    out[*len] = '\0';
}

static void build_wifi_qr_text(char *out, size_t out_size, const char *ssid, const char *password) {
    size_t len = 0;
    out[0] = '\0';
    snprintf(out, out_size, "WIFI:T:WPA;S:");
    len = strlen(out);
    append_escaped_wifi_field(out, out_size, &len, ssid);
    if (len + 3 < out_size) {
        memcpy(out + len, ";P:", 3);
        len += 3;
        out[len] = '\0';
    }
    append_escaped_wifi_field(out, out_size, &len, password);
    if (len + 2 < out_size) {
        memcpy(out + len, ";;", 2);
        len += 2;
        out[len] = '\0';
    }
}

// 把 QR code 縮放畫到 framebuffer 上，(x0,y0) 是左上角、target_size_px 是希望
// 佔用的邊長（實際邊長會是 module 數的整數倍，可能略小於這個值）。四周留白
// （quiet zone）不用額外處理——整個畫面一開始就被 Paint_Clear(WHITE) 清成
// 白色，只要旁邊的文字別畫得太靠近就自然留白。
static void draw_qr_code(const uint8_t *qr, int x0, int y0, int target_size_px) {
    int modules = qrcodegen_getSize(qr);
    int scale = target_size_px / modules;
    if (scale < 1) {
        scale = 1;
    }
    for (int y = 0; y < modules; y++) {
        for (int x = 0; x < modules; x++) {
            UWORD color = qrcodegen_getModule(qr, x, y) ? BLACK : WHITE;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    Paint_SetPixel((UWORD)(x0 + x * scale + dx), (UWORD)(y0 + y * scale + dy), color);
                }
            }
        }
    }
}

void display_status_show_ap_config(const char *ap_ssid, const char *ap_password,
                                    const device_config_t *existing_config) {
    s_ble_screen_is_current = false;
    begin_frame();

    Paint_DrawString_EN(5, 2, "WiFi Setup", &Font16, BLACK, WHITE);

    // SSID/密碼用 Font8（比 Font12 窄）畫，QR code 從 x=180 開始（見下方
    // draw_qr_code() 呼叫），左側文字只有 175px 寬可用——SSID 帶了 MAC 衍生
    // 的字尾（例如 "PicoGateway-Setup-DC15"）長度會超過 Font12 在這個寬度
    // 塞得下的字元數，換成 Font8 才不會被 QR code 蓋到/裁切掉。
    char line[48];
    snprintf(line, sizeof(line), "SSID: %s", ap_ssid);
    Paint_DrawString_EN(5, 26, line, &Font8, BLACK, WHITE);
    snprintf(line, sizeof(line), "Pass: %s", ap_password);
    Paint_DrawString_EN(5, 38, line, &Font8, BLACK, WHITE);

    char patient_id_ascii[PATIENT_ID_MAX_LEN];
    if (existing_config != NULL && existing_config->valid) {
        sanitize_ascii(existing_config->patient_id, patient_id_ascii, sizeof(patient_id_ascii));
    } else {
        patient_id_ascii[0] = '\0';
    }
    snprintf(line, sizeof(line), "ID: %s", patient_id_ascii[0] != '\0' ? patient_id_ascii : "(unset)");
    Paint_DrawString_EN(5, 60, line, &Font12, BLACK, WHITE);

    Paint_DrawString_EN(5, 90, "Scan QR or connect", &Font12, BLACK, WHITE);
    Paint_DrawString_EN(5, 104, "phone to hotspot ->", &Font12, BLACK, WHITE);

    // QR code 放右側。字串很短（SSID+密碼加起來不到 50 bytes），版本上限給 10
    // 綽綽有餘（能放到數百字元），buffer 用得到的空間遠小於這個上限對應的大小。
    char wifi_text[160];
    build_wifi_qr_text(wifi_text, sizeof(wifi_text), ap_ssid, ap_password);

    static uint8_t s_qr_temp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    static uint8_t s_qr_code[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    bool ok = qrcodegen_encodeText(wifi_text, s_qr_temp, s_qr_code, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);
    if (ok) {
        draw_qr_code(s_qr_code, 180, 8, 112);
    } else {
        printf("[DISPLAY] qrcodegen_encodeText() failed, skipping WiFi QR code\n");
        Paint_DrawString_EN(180, 40, "(QR failed)", &Font12, BLACK, WHITE);
    }

    end_frame_and_refresh();
}

void display_status_show_upload(const char *ssid, const char *result_text) {
    s_ble_screen_is_current = false;
    begin_frame();

    Paint_DrawString_EN(5, 2, "Uploading", &Font16, BLACK, WHITE);

    char line[48];
    snprintf(line, sizeof(line), "WiFi: %s", ssid != NULL ? ssid : "?");
    Paint_DrawString_EN(5, 30, line, &Font12, BLACK, WHITE);
    Paint_DrawString_EN(5, 46, result_text, &Font12, BLACK, WHITE);

    end_frame_and_refresh();
}

void display_status_show_error(const char *message) {
    s_ble_screen_is_current = false;
    begin_frame();

    Paint_DrawString_EN(5, 2, "ERROR", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(5, 30, message, &Font12, BLACK, WHITE);

    end_frame_and_refresh();
}

// ---------------------------------------------------------------------------
// BLE_RECEIVE：內容會在一個持續數十秒到數分鐘的緊迴圈裡持續變動（新讀值、
// 待傳筆數），需要非阻塞的「內容變了才刷新」機制，見 display_status_poll()。
// ---------------------------------------------------------------------------

// 目前想顯示的內容（由 display_status_set_ble_receive() 寫入，poll() 讀取）。
static device_config_t s_ble_config;
static bool s_ble_have_config = false;
static char s_ble_status_text[32] = "";

// 上一次實際畫到螢幕上的內容快照，用來判斷這次要不要刷新。刻意不包含任何
// 「距今多久」這種會隨時間漂移的文字——只比對原始資料（數值/時間戳/筆數/
// 狀態文字/個案編號），這樣畫面只在真的有新事件（新讀值、待傳筆數變化、
// 狀態文字改變）時才刷新，不會被時間流逝本身觸發（"3 min ago" 這類文字
// 顯示的是「上次刷新當下」算出來的，不是即時的，這是刻意的簡化，因為 EPD
// 全刷要 3 秒、不能為了讓文字即時而頻繁刷新）。
typedef struct {
    bool initialized;
    char status_text[32];
    char patient_id_ascii[PATIENT_ID_MAX_LEN];
    size_t pending_count;
    bool reading_valid[VITAL_TYPE_COUNT];
    float reading_value[VITAL_TYPE_COUNT];
    uint64_t reading_received_at_ms[VITAL_TYPE_COUNT];
    // 裝置自己回報的量測時間戳（見 common.h vital_record_t.device_measured_key
    // 的說明），0 代表這種裝置的協定沒有這個資訊，畫面上要退回顯示
    // reading_received_at_ms。目前只有血壓計有值。
    uint32_t reading_device_measured_key[VITAL_TYPE_COUNT];
    bool last_upload_valid;
    uint64_t last_upload_at_ms;
} ble_snapshot_t;

static ble_snapshot_t s_last_rendered;

static void capture_current_snapshot(ble_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->initialized = true;
    strncpy(out->status_text, s_ble_status_text, sizeof(out->status_text) - 1);

    if (s_ble_have_config && s_ble_config.valid) {
        sanitize_ascii(s_ble_config.patient_id, out->patient_id_ascii, sizeof(out->patient_id_ascii));
    }

    out->pending_count = storage_pending_count();

    for (vital_type_t type = VITAL_TYPE_TEMPERATURE; type < VITAL_TYPE_COUNT; type++) {
        vital_record_t record;
        if (storage_get_last_reading(type, &record)) {
            out->reading_valid[type] = true;
            out->reading_value[type] = record.value;
            out->reading_received_at_ms[type] = record.received_at_ms;
            out->reading_device_measured_key[type] = record.device_measured_key;
        }
    }

    out->last_upload_valid = storage_get_last_upload_time(&out->last_upload_at_ms);
}

static bool snapshots_equal(const ble_snapshot_t *a, const ble_snapshot_t *b) {
    if (a->initialized != b->initialized) {
        return false;
    }
    if (strcmp(a->status_text, b->status_text) != 0) {
        return false;
    }
    if (strcmp(a->patient_id_ascii, b->patient_id_ascii) != 0) {
        return false;
    }
    if (a->pending_count != b->pending_count) {
        return false;
    }
    for (int i = 0; i < VITAL_TYPE_COUNT; i++) {
        if (a->reading_valid[i] != b->reading_valid[i]) {
            return false;
        }
        if (a->reading_valid[i] && (a->reading_value[i] != b->reading_value[i] ||
                                     a->reading_received_at_ms[i] != b->reading_received_at_ms[i] ||
                                     a->reading_device_measured_key[i] != b->reading_device_measured_key[i])) {
            return false;
        }
    }
    if (a->last_upload_valid != b->last_upload_valid) {
        return false;
    }
    if (a->last_upload_valid && a->last_upload_at_ms != b->last_upload_at_ms) {
        return false;
    }
    return true;
}

static const char *vital_label(vital_type_t type) {
    switch (type) {
        case VITAL_TYPE_TEMPERATURE: return "Temp ";
        case VITAL_TYPE_SPO2:        return "SpO2 ";
        case VITAL_TYPE_PULSE_RATE:  return "Pulse";
        case VITAL_TYPE_GLUCOSE:     return "Gluc ";
        case VITAL_TYPE_SYSTOLIC:    return "Sys  ";
        case VITAL_TYPE_DIASTOLIC:   return "Dia  ";
        default:                     return "?    ";
    }
}

static const char *vital_unit(vital_type_t type) {
    switch (type) {
        case VITAL_TYPE_TEMPERATURE: return "C";
        case VITAL_TYPE_SPO2:        return "%";
        case VITAL_TYPE_PULSE_RATE:  return "bpm";
        case VITAL_TYPE_GLUCOSE:     return "mg/dL";
        case VITAL_TYPE_SYSTOLIC:    return "mmHg";
        case VITAL_TYPE_DIASTOLIC:   return "mmHg";
        default:                     return "";
    }
}

// 有些裝置（目前是血壓計）的回應本身帶有裝置自己認證過的量測時間（見
// common.h vital_record_t.device_measured_key 的說明），這種情況畫面上要顯示
// 「裝置量測當下」的時間，不是「Pico 收到 BLE 通知」的時間——裝置可能在量測
// 完之後過一段時間才被 Pico 連上、讀到資料，兩個時間點不一定相同。沒有這個
// 資訊的裝置（額溫槍/血氧計，key==0）才退回顯示 received_at_ms。
static void format_reading_clock(uint64_t received_at_ms, uint32_t device_measured_key,
                                  char *out, size_t out_size) {
    if (device_measured_key != 0) {
        unsigned year, month, day, hour, minute;
        fora_protocol_measured_key_to_datetime(device_measured_key, &year, &month, &day, &hour, &minute);
        snprintf(out, out_size, "%02u/%02u %02u:%02u", month, day, hour, minute);
        return;
    }
    display_status_format_clock(received_at_ms, out, out_size);
}

static void draw_reading_row(int y, vital_type_t type, const ble_snapshot_t *snap) {
    char line[48];
    if (snap->reading_valid[type]) {
        char value_str[16];
        format_vital_value(value_str, sizeof(value_str), type, snap->reading_value[type]);
        char clock_str[24];
        format_reading_clock(snap->reading_received_at_ms[type], snap->reading_device_measured_key[type],
                              clock_str, sizeof(clock_str));
        snprintf(line, sizeof(line), "%s %s%s (%s)", vital_label(type), value_str, vital_unit(type), clock_str);
    } else {
        snprintf(line, sizeof(line), "%s -- (never)", vital_label(type));
    }
    Paint_DrawString_EN(5, y, line, &Font12, BLACK, WHITE);
}

static bool render_ble_receive(const ble_snapshot_t *snap) {
    begin_frame();

    char line[48];
    snprintf(line, sizeof(line), "ID: %s", snap->patient_id_ascii[0] != '\0' ? snap->patient_id_ascii : "(unset)");
    Paint_DrawString_EN(5, 2, line, &Font12, BLACK, WHITE);
    Paint_DrawString_EN(5, 16, snap->status_text[0] != '\0' ? snap->status_text : "Idle", &Font12, BLACK, WHITE);

    draw_reading_row(36, VITAL_TYPE_TEMPERATURE, snap);
    draw_reading_row(49, VITAL_TYPE_SPO2, snap);
    draw_reading_row(62, VITAL_TYPE_PULSE_RATE, snap);
    draw_reading_row(75, VITAL_TYPE_GLUCOSE, snap);

    // 血壓收縮/舒張合成一行顯示；兩者通常同一次量測一起寫入，時間戳取收縮壓的。
    if (snap->reading_valid[VITAL_TYPE_SYSTOLIC] && snap->reading_valid[VITAL_TYPE_DIASTOLIC]) {
        char clock_str[24];
        format_reading_clock(snap->reading_received_at_ms[VITAL_TYPE_SYSTOLIC],
                              snap->reading_device_measured_key[VITAL_TYPE_SYSTOLIC], clock_str, sizeof(clock_str));
        snprintf(line, sizeof(line), "BP    %d/%d mmHg (%s)",
                 (int)(snap->reading_value[VITAL_TYPE_SYSTOLIC] + 0.5f),
                 (int)(snap->reading_value[VITAL_TYPE_DIASTOLIC] + 0.5f), clock_str);
    } else {
        snprintf(line, sizeof(line), "BP    -- (never)");
    }
    Paint_DrawString_EN(5, 88, line, &Font12, BLACK, WHITE);

    if (snap->last_upload_valid) {
        char clock_str[24];
        display_status_format_clock(snap->last_upload_at_ms, clock_str, sizeof(clock_str));
        snprintf(line, sizeof(line), "Last upload: %s", clock_str);
    } else {
        snprintf(line, sizeof(line), "Last upload: never");
    }
    Paint_DrawString_EN(5, 101, line, &Font12, BLACK, WHITE);

    snprintf(line, sizeof(line), "Pending uploads: %u", (unsigned)snap->pending_count);
    Paint_DrawString_EN(5, 114, line, &Font12, BLACK, WHITE);

    return end_frame_and_refresh();
}

void display_status_set_ble_receive(const device_config_t *config, const char *status_text) {
    if (config != NULL) {
        s_ble_config = *config;
        s_ble_have_config = true;
    }
    if (status_text != NULL) {
        strncpy(s_ble_status_text, status_text, sizeof(s_ble_status_text) - 1);
        s_ble_status_text[sizeof(s_ble_status_text) - 1] = '\0';
    }
}

void display_status_poll(void) {
    ble_snapshot_t current;
    capture_current_snapshot(&current);

    bool content_changed = !snapshots_equal(&current, &s_last_rendered);
    // 面板規格要求「至少每 24 小時刷新一次」，就算內容完全沒變也一樣——這是
    // BLE_RECEIVE 這個 24/7 常駐模式才需要處理的情況（AP_CONFIG/UPLOAD 執行
    // 時間本來就短，不會連續跑超過 24 小時）。
    bool refresh_overdue = s_have_refreshed_once &&
        absolute_time_diff_us(s_last_actual_refresh, get_absolute_time()) / 1000 >= (int64_t)MAX_REFRESH_INTERVAL_MS;

    // 中間顯示過別的畫面（AP_CONFIG/UPLOAD/錯誤）的話，就算這次 BLE_RECEIVE
    // 的內容跟切走前最後一次畫的一模一樣，也一定要強制刷新一次——不然面板會
    // 一直停在舊畫面（例如「Uploading...Failed」），使用者以為裝置卡住，
    // 見 s_ble_screen_is_current 宣告處的說明。
    if (!content_changed && !refresh_overdue && s_ble_screen_is_current) {
        return; // 內容沒變、也還沒到強制刷新的時間、畫面本來就是這個，不刷新
    }

    // render_ble_receive() 目前恆回傳 true；若之後改成有條件跳過刷新，
    // s_last_rendered 應該只在真的刷新成功時才更新，讓下一輪 poll() 繼續
    // 認定內容「還沒真的畫上去」，不會遺失這次要顯示的內容。
    if (render_ble_receive(&current)) {
        s_last_rendered = current;
        s_ble_screen_is_current = true;
    }
}

#define HISTORY_DISPLAY_MAX_ROWS 7

void display_status_show_upload_history(const vital_record_t *records, size_t count, size_t total_count) {
    s_ble_screen_is_current = false;
    begin_frame();

    char line[48];
    snprintf(line, sizeof(line), "Upload History (%u total)", (unsigned)total_count);
    Paint_DrawString_EN(5, 2, line, &Font12, BLACK, WHITE);

    if (count == 0) {
        Paint_DrawString_EN(5, 20, "(none yet)", &Font12, BLACK, WHITE);
    } else {
        int y = 18;
        size_t rows = count < HISTORY_DISPLAY_MAX_ROWS ? count : HISTORY_DISPLAY_MAX_ROWS;
        for (size_t i = 0; i < rows; i++) {
            char value_str[16];
            format_vital_value(value_str, sizeof(value_str), records[i].type, records[i].value);
            char clock_str[24];
            format_reading_clock(records[i].received_at_ms, records[i].device_measured_key,
                                  clock_str, sizeof(clock_str));
            snprintf(line, sizeof(line), "%s %s%s (%s)", vital_label(records[i].type), value_str,
                     vital_unit(records[i].type), clock_str);
            Paint_DrawString_EN(5, y, line, &Font12, BLACK, WHITE);
            y += 13;
        }
    }

    end_frame_and_refresh();
}
