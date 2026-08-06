#include "mode_ble_receive.h"

#include "button_input.h"
#include "common.h"
#include "display_status.h"
#include "fora_protocol.h"
#include "led_status.h"
#include "storage.h"
#include "wall_clock.h"

#include "btstack.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

// KEY0 需要連續按住這麼久才會觸發進入熱點設定模式（見 button_input.h），
// 避免不小心碰到就誤觸發；跟開機時「按住 BOOTSEL」的窗口時間量級一致。
#define KEY0_ENTER_CONFIG_HOLD_MS 3000

// KEY2 觸發已上傳歷史畫面，顯示這麼久之後自動換回 BLE_RECEIVE 即時畫面。
#define KEY2_HISTORY_VIEW_MS 8000
#define KEY2_HISTORY_DISPLAY_ROWS 7

// 還沒校時成功時，每隔這麼久主動連一次 WiFi 重試 NTP，不等待收到裝置讀值
// 才觸發（見主迴圈裡的說明）。校時成功後這個計時器就不會再觸發。
#define NTP_UNSYNCED_RETRY_MS (5 * 60 * 1000)

// 狀態機結構參考 BTstack 範例 lib/btstack/example/gatt_heart_rate_client.c：
// 掃描 -> 連線 -> 探索服務 -> 探索特徵值 -> 訂閱通知 -> 持續接收。
typedef enum {
    BLE_STATE_IDLE,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_DISCOVER_SERVICE,
    BLE_STATE_DISCOVER_CHARACTERISTIC,
    BLE_STATE_ENABLE_NOTIFY,
    BLE_STATE_LISTENING,
    BLE_STATE_PAIRING,
} ble_receive_state_t;

static btstack_packet_callback_registration_t s_hci_event_callback_registration;
static btstack_packet_callback_registration_t s_sm_event_callback_registration;
static hci_con_handle_t s_connection_handle = HCI_CON_HANDLE_INVALID;
static gatt_client_service_t s_fora_service;
static gatt_client_characteristic_t s_fora_characteristic;
static gatt_client_notification_t s_notification_listener;

static ble_receive_state_t s_ble_state = BLE_STATE_IDLE;
static volatile bool s_connected_and_ready = false;
static absolute_time_t s_last_reading_at;
static uint32_t s_idle_timeout_ms;

// 這一輪正在連線/已連線的是哪一種 FORA 裝置（額溫槍/血氧計），從掃描比對
// 廣播封包時判斷出來，後面連線、探索、快取 handle 都要用同一個值分開處理——
// 兩種裝置雖然用同一套 UUID，但各自的 GATT attribute table 排列不同
// （血氧計前面多了好幾個標準服務），handle 編號並不通用，混用會查詢失敗。
static fora_device_kind_t s_current_kind = FORA_DEVICE_UNKNOWN;

// 每種裝置拿到讀值之後，多久內不要再重新連線同一種裝置，讓裝置有機會走到
// 它自己的休眠邏輯。這個冷卻時間**不是**每次進 BLE_RECEIVE 模式就重置，是
// 跨越 BLE_RECEIVE/UPLOAD 模式切換持續有效的。依裝置種類分開追蹤，額溫槍的
// 冷卻不影響血氧計、反之亦然。判重邏輯不依賴冷卻時間長短（靠裝置端時間戳/
// 數值比對，見 storage.c 的 storage_append_record()），冷卻時間只要「夠長、
// 能讓裝置真的睡著」就好：
//   額溫槍：官方休眠門檻 1 分鐘 → 冷卻設 60 秒。
//   血壓計：官方休眠門檻 3 分鐘 → 冷卻設 4 分鐘（240 秒，多留 1 分鐘餘裕）。
//   血氧計：官方休眠門檻未知，5 秒是暫定值，見 PROJECT_PLAN.md 第 7 節。
static const uint32_t DEVICE_RECONNECT_COOLDOWN_MS[FORA_DEVICE_KIND_COUNT] = {
    [FORA_DEVICE_UNKNOWN] = 0,
    [FORA_DEVICE_THERMOMETER] = 60 * 1000,
    [FORA_DEVICE_OXIMETER] = 5 * 1000,
    [FORA_DEVICE_BLOOD_PRESSURE] = 240 * 1000,
};
static absolute_time_t s_kind_cooldown_until[FORA_DEVICE_KIND_COUNT];

// 這一輪（這次進入 BLE_RECEIVE 到現在）有沒有收到過任何一筆生理訊號？在收到
// 第一筆之前，idle timeout 不該開始算——像血壓計整個充放氣量測要 30-45 秒，
// 從「進入 BLE_RECEIVE」那一刻就開始倒數的話，量測還沒做完就會被切到 UPLOAD
// 模式、逼著斷線。要切模式的判斷是「收到訊號後過了多久沒有新訊號」，不是
// 「進入這個模式後過了多久」。
static bool s_got_any_reading_this_session = false;

// FORA 裝置量測完只會短暫連線一下就主動斷線，留給我們做完整服務/特徵值探索
// 的時間可能不夠。裝置的 GATT attribute table 在多次連線之間是固定的，所以
// 探索一次後就快取 handle，下次連線直接跳去訂閱，省掉兩次 ATT 來回，盡量在
// 裝置斷線前完成訂閱。依裝置種類分開快取（原因見上面 s_current_kind 的說明）。
typedef struct {
    bool cached;
    gatt_client_service_t service;
    gatt_client_characteristic_t characteristic;
} fora_handle_cache_t;
static fora_handle_cache_t s_handle_cache[FORA_DEVICE_KIND_COUNT];

// 血壓計「問記錄」的兩段式交換（見 fora_protocol.h 的協定說明）：第一段回應
// 先暫存在這裡，等第二段回應收到後才跟它接成 8 bytes 一起解析。
static uint8_t s_bp_record_part_a[4];
static bool s_bp_waiting_for_part_b = false;

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// 畫面顯示用的個案設定，開機/每次進入這個模式時讀一次 flash 就好（讀取本身
// 很便宜），不需要每輪迴圈重讀。設定還沒存過（例如從沒進過 AP_CONFIG）的話
// display_status_set_ble_receive() 會收到 NULL，畫面上 ID 那欄會顯示 "(unset)"。
static device_config_t s_display_config;
static bool s_have_display_config = false;

// 上一次更新「Scanning」狀態文字的時間戳，讓 mode_ble_receive_run() 的主迴圈
// 可以定期（不需要等掃到裝置才觸發）重新整理畫面上的時間戳，見
// update_scanning_status() 的說明。
static absolute_time_t s_last_scanning_status_update_at;

// 上一次嘗試「還沒校時成功就主動重試 NTP」的時間戳，見主迴圈裡 NTP_UNSYNCED_RETRY_MS
// 的檢查。**故意不在 mode_ble_receive_run() 開頭重置**（跟 s_kind_cooldown_until[]
// 的道理一樣）：零值（開機時的預設值）代表「無窮久以前」，讓開機後第一次檢查
// 就會成立、立刻嘗試一次校時，不用空等滿一整個 NTP_UNSYNCED_RETRY_MS 週期；
// 之後每次真的觸發重試時才會更新這個時間戳，重新開始計算下一次的間隔。
static absolute_time_t s_last_ntp_retry_at;

// KEY2 觸發歷史畫面期間暫停呼叫 display_status_poll()，不然畫面會馬上被
// BLE_RECEIVE 即時內容蓋掉，見主迴圈裡的說明。
static bool s_showing_history = false;
static absolute_time_t s_history_view_until;

// 組出「Scanning (last: HH:MM)」這種帶時間戳的狀態文字並更新畫面
// （display_status_set_ble_receive() 本身不會馬上刷新面板，實際刷新由
// display_status_poll() 內容比對後決定，見 display_status.h 的說明）。
// 這裡的時間戳不是「上次掃到裝置的時間」，是「這段文字被組出來那一刻的時間」
// ——用意是讓使用者能從畫面判斷「裝置還活著、只是沒掃到新裝置」跟「裝置已經
// 當機、畫面凍結」的差別（見 display_status_set_ble_receive() 的說明），跟
// start_scan() 一起呼叫，並且在主迴圈裡定期呼叫讓時間戳持續前進。
static void update_scanning_status(void) {
    char status_text[32];
    char clock_str[16];
    display_status_format_clock(to_ms_since_boot(get_absolute_time()), clock_str, sizeof(clock_str));
    snprintf(status_text, sizeof(status_text), "Scanning (%s)", clock_str);
    display_status_set_ble_receive(s_have_display_config ? &s_display_config : NULL, status_text);
    s_last_scanning_status_update_at = get_absolute_time();
}

static void start_scan(void) {
    s_ble_state = BLE_STATE_SCANNING;
    led_status_set(LED_SLOW_BLINK);
    update_scanning_status();
    // scan_type=1 使用主動掃描（會送 SCAN_REQ 換 SCAN_RESPONSE）。很多裝置把
    // 裝置名稱放在 scan response 而非主要廣播封包，被動掃描(0)會看不到名稱。
    gap_set_scan_parameters(1, 0x0030, 0x0030);
    gap_start_scan();
}

// 這一輪服務/特徵值探索有沒有真的找到符合的 UUID——GATT_EVENT_QUERY_COMPLETE
// 只代表查詢本身正常跑完，不代表有找到結果（陌生裝置沒有這個 service/
// characteristic 時，查詢一樣會「成功」完成，但完全沒有結果）。每次開始探索
// 前重置，收到對應的 QUERY_RESULT 事件才設成 true。
static bool s_discovery_found = false;

static void discover_service(void) {
    s_ble_state = BLE_STATE_DISCOVER_SERVICE;
    s_discovery_found = false;
    // 三種裝置都走同一個 Nordic LED/Button Service 自訂 128-bit UUID pipe
    // （見 fora_protocol.h 開頭的說明），用同一套探索方式。
    gatt_client_discover_primary_services_by_uuid128(
        handle_gatt_client_event, s_connection_handle, FORA_SERVICE_UUID128);
}

static void discover_characteristic(void) {
    s_ble_state = BLE_STATE_DISCOVER_CHARACTERISTIC;
    s_discovery_found = false;
    gatt_client_discover_characteristics_for_service_by_uuid128(
        handle_gatt_client_event, s_connection_handle, &s_fora_service, FORA_CHARACTERISTIC_UUID128);
}

// 三種裝置都走同一套自訂 pipe 的 Notify 機制。
static void enable_value_updates(void) {
    s_ble_state = BLE_STATE_ENABLE_NOTIFY;
    gatt_client_listen_for_characteristic_value_updates(
        &s_notification_listener, handle_gatt_client_event, s_connection_handle, &s_fora_characteristic);
    gatt_client_write_client_characteristic_configuration(
        handle_gatt_client_event, s_connection_handle, &s_fora_characteristic,
        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
}

// 訂閱成功後，裝置不會自動推播，要主動寫入觸發指令才會回傳目前量到的數值
// （已用舊版可動的 MicroPython 實作確認）。用 write-without-response，
// 不需要等待 ATT 回應。
static void send_trigger_command(void) {
    gatt_client_write_value_of_characteristic_without_response(
        s_connection_handle, s_fora_characteristic.value_handle,
        sizeof(FORA_TRIGGER_COMMAND), (uint8_t *)FORA_TRIGGER_COMMAND);
}

// 血壓計「問目前這筆記錄」的兩段式交換第一/二段（見 fora_protocol.h 的協定
// 說明）：cmd 只會是 FORA_BP_CMD_GET_RECORD_PART_A 或 _PART_B，索引固定填 0
// （最新一筆），使用者編號固定用 FORA_BP_USER_CURRENT——這台裝置這兩個假設
// 目前都還沒有機會驗證是不是所有情況都成立，見 PROJECT_PLAN.md 12 節的說明。
static void send_bp_get_record_part(uint8_t cmd) {
    uint8_t command[8];
    fora_protocol_build_command(cmd, 0x00, 0x00, 0x00, FORA_BP_USER_CURRENT, command);
    gatt_client_write_value_of_characteristic_without_response(
        s_connection_handle, s_fora_characteristic.value_handle, sizeof(command), command);
}

// 三種裝置的 Notify payload 都送進這裡解析、存起來、斷線——共用同一份邏輯。
static void process_reading_payload(const uint8_t *value, uint16_t value_len) {
    vital_record_t records[FORA_MAX_READINGS_PER_NOTIFICATION];
    size_t record_count = fora_protocol_parse_reading(s_current_kind, value, value_len, records);
    if (record_count == 0) {
        printf("[BLE] fora_protocol_parse_reading() returned 0 (payload 格式不符預期).\n");
        return;
    }

    uint64_t now_ms = to_ms_since_boot(get_absolute_time());
    for (size_t i = 0; i < record_count; i++) {
        records[i].received_at_ms = now_ms;
        records[i].status = UPLOAD_STATUS_PENDING;
        storage_append_record(&records[i]);
        // 手動格式化浮點數，避免依賴 newlib-nano 預設未啟用的 printf float 支援；
        // 用四捨五入到小數點後 1 位，不是無條件捨去。
        int tenths = (int)(records[i].value * 10.0f + (records[i].value >= 0.0f ? 0.5f : -0.5f));
        int whole = tenths / 10;
        int frac = tenths % 10;
        if (frac < 0) {
            frac = -frac;
        }
        printf("[BLE] parsed reading: type=%d value=%d.%d\n", records[i].type, whole, frac);
    }
    s_last_reading_at = get_absolute_time();
    s_got_any_reading_this_session = true;
    s_kind_cooldown_until[s_current_kind] = make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS[s_current_kind]);
    led_status_set(LED_HEARTBEAT);
    // 已經拿到這次量測的數值，主動斷線，回到掃描狀態等下一次量測。
    gap_disconnect(s_connection_handle);
}

// 已知的 FORA IR42 位址，debug log 用來直接鎖定這顆裝置，不只靠裝置名稱
// 判斷──manufacturer data 可能出現在沒有帶名稱的廣播封包裡。
static const bd_addr_t FORA_KNOWN_ADDR = { 0xC0, 0x26, 0xDA, 0x28, 0xB6, 0xE6 };

// 除錯用：把掃描到的每個裝置名稱/位址/manufacturer data 印出來。
static void debug_print_advertisement(uint8_t *packet, const uint8_t *adv_data, uint8_t adv_len) {
    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    int8_t rssi = gap_event_advertising_report_get_rssi(packet);

    char name[32];
    name[0] = '\0';
    const uint8_t *mfg_data = NULL;
    uint8_t mfg_len = 0;

    ad_context_t context;
    for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        uint8_t data_len = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);

        if (data_type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
            data_type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
            uint8_t copy_len = data_len < sizeof(name) - 1 ? data_len : sizeof(name) - 1;
            memcpy(name, data, copy_len);
            name[copy_len] = '\0';
        } else if (data_type == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
            mfg_data = data;
            mfg_len = data_len;
        }
    }

    bool is_fora = strstr(name, "FORA") != NULL || memcmp(addr, FORA_KNOWN_ADDR, sizeof(bd_addr_t)) == 0;
    if (!is_fora) {
        return;
    }

    printf("[BLE scan] addr=%s rssi=%d name=\"%s\" mfg_data=", bd_addr_to_str(addr), rssi, name);
    if (mfg_data == NULL) {
        printf("(none)\n");
    } else {
        for (uint8_t i = 0; i < mfg_len; i++) {
            printf("%02x ", mfg_data[i]);
        }
        printf("\n");
    }
}

static void handle_advertising_report(uint8_t *packet) {
    if (s_ble_state != BLE_STATE_SCANNING) {
        return;
    }

    const uint8_t *adv_data = gap_event_advertising_report_get_data(packet);
    uint8_t adv_len = gap_event_advertising_report_get_data_length(packet);

    debug_print_advertisement(packet, adv_data, adv_len);

    fora_device_kind_t kind = FORA_DEVICE_UNKNOWN;
    if (!fora_protocol_matches_advertisement(adv_data, adv_len, &kind)) {
        return;
    }

    if (!time_reached(s_kind_cooldown_until[kind])) {
        return; // 這種裝置還在冷卻時間內，不要再打擾它
    }

    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);

    s_current_kind = kind;
    printf("[BLE] matched FORA device %s (kind=%d), connecting...\n", bd_addr_to_str(addr), kind);

    gap_stop_scan();
    s_ble_state = BLE_STATE_CONNECTING;
    gap_connect(addr, addr_type);
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &s_fora_service);
            s_discovery_found = true;
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(packet, &s_fora_characteristic);
            s_discovery_found = true;
            break;

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t att_status = gatt_event_query_complete_get_att_status(packet);
            bool is_discovery_state = s_ble_state == BLE_STATE_DISCOVER_SERVICE ||
                                       s_ble_state == BLE_STATE_DISCOVER_CHARACTERISTIC;
            if (att_status != ATT_ERROR_SUCCESS || (is_discovery_state && !s_discovery_found)) {
                // 查詢本身可能「成功」完成卻完全沒有結果（陌生裝置沒有這個
                // service/characteristic），這種情況不能當成正常繼續往下走，
                // 不然會拿沒填過的 s_fora_service/s_fora_characteristic 去用。
                printf("[BLE] query failed or empty at state=%d, att_status=0x%02x, disconnecting.\n",
                       s_ble_state, att_status);
                gap_disconnect(s_connection_handle);
                break;
            }
            if (s_ble_state == BLE_STATE_DISCOVER_SERVICE) {
                printf("[BLE] service discovered, looking up characteristic...\n");
                discover_characteristic();
            } else if (s_ble_state == BLE_STATE_DISCOVER_CHARACTERISTIC) {
                printf("[BLE] characteristic discovered...\n");
                // 存進這種裝置專屬的快取，下次連上同種裝置可以直接跳過探索。
                s_handle_cache[s_current_kind].cached = true;
                s_handle_cache[s_current_kind].service = s_fora_service;
                s_handle_cache[s_current_kind].characteristic = s_fora_characteristic;
                enable_value_updates();
            } else if (s_ble_state == BLE_STATE_ENABLE_NOTIFY) {
                s_ble_state = BLE_STATE_LISTENING;
                s_connected_and_ready = true;
                // 不特地把螢幕狀態文字換成「Connected, reading...」——連線到拿到
                // 讀值通常不到 1 秒（見 PROJECT_PLAN.md 實測 log），這個中間狀態
                // 螢幕上根本來不及被人看到就會被下一個事件（拿到讀值／逾時)
                // 蓋掉，卻會多刷新一次面板（全刷要 3 秒），對使用者只有壞處沒有
                // 好處。保留狀態文字不變，等真的收到新讀值那一刻，
                // process_reading_payload() 之後 poll() 自然會因為讀值/時間戳
                // 改變而刷新一次，那次才是使用者真正在意的內容。
                printf("[BLE] value updates enabled (att_status=0x%02x)\n", att_status);
                if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
                    // 血壓計走跟額溫槍/血氧計一樣的自訂 pipe，但要用「問記錄」
                    // 的兩段式交換取值（見 fora_protocol.h 的協定說明），不是
                    // 單一次觸發指令。這裡先送第一段，第二段在收到第一段回應
                    // 後才送（見 GATT_EVENT_NOTIFICATION 那邊的處理）。
                    s_bp_waiting_for_part_b = false;
                    printf("[BLE] requesting latest BP record (part A)...\n");
                    send_bp_get_record_part(FORA_BP_CMD_GET_RECORD_PART_A);
                } else {
                    printf("[BLE] sending trigger command...\n");
                    send_trigger_command();
                }
            }
            break;
        }

        case GATT_EVENT_NOTIFICATION: {
            const uint8_t *value = gatt_event_notification_get_value(packet);
            uint16_t value_len = gatt_event_notification_get_value_length(packet);
            printf("[BLE] notification received (%u bytes):", value_len);
            for (uint16_t i = 0; i < value_len; i++) {
                printf(" %02x", value[i]);
            }
            printf("\n");

            if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
                if (value_len < 6 || value[0] != 0x51) {
                    printf("[BLE] unexpected BP response, ignoring.\n");
                    break;
                }
                if (!s_bp_waiting_for_part_b) {
                    // 第一段回應：存起來，馬上送第二段——這兩段合起來才是完整
                    // 的一筆記錄，見 fora_protocol.h 的協定說明。
                    memcpy(s_bp_record_part_a, &value[2], sizeof(s_bp_record_part_a));
                    s_bp_waiting_for_part_b = true;
                    printf("[BLE] got record part A, requesting part B...\n");
                    send_bp_get_record_part(FORA_BP_CMD_GET_RECORD_PART_B);
                } else {
                    // 第二段回應：跟第一段接成 8 bytes，交給共用的解析/儲存邏輯。
                    uint8_t combined[8];
                    memcpy(&combined[0], s_bp_record_part_a, sizeof(s_bp_record_part_a));
                    memcpy(&combined[4], &value[2], 4);
                    process_reading_payload(combined, sizeof(combined));
                }
                break;
            }

            process_reading_payload(value, value_len);
            break;
        }

        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
            // 目前沒有任何裝置會主動觸發 Read，三種裝置都是靠 Notify 推播
            // 取值。這個 case 保留著給以後可能需要用 Read 的裝置共用同一套
            // 解析流程。
            const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet);
            uint16_t value_len = gatt_event_characteristic_value_query_result_get_value_length(packet);
            printf("[BLE] read value (%u bytes):", value_len);
            for (uint16_t i = 0; i < value_len; i++) {
                printf(" %02x", value[i]);
            }
            printf("\n");
            process_reading_payload(value, value_len);
            break;
        }

        default:
            break;
    }
}

// 血壓計連線後先配對，配對完成後才根據有沒有快取 handle，決定直接訂閱
// Notify 還是先做服務/特徵值探索。
static void proceed_after_pairing(void) {
    if (s_handle_cache[s_current_kind].cached) {
        s_fora_service = s_handle_cache[s_current_kind].service;
        s_fora_characteristic = s_handle_cache[s_current_kind].characteristic;
        enable_value_updates();
    } else {
        discover_service();
    }
}

static void handle_sm_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            // 這裝置沒有螢幕/按鍵，Just Works 直接確認即可，不需要比對數字/輸入密碼。
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE] pairing complete, proceeding...\n");
                proceed_after_pairing();
            } else {
                printf("[BLE] pairing failed (status=0x%02x), disconnecting.\n", status);
                gap_disconnect(s_connection_handle);
            }
            break;
        }

        default:
            break;
    }
}

static void handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    uint8_t event_type = hci_event_packet_get_type(packet);

    if (event_type == BTSTACK_EVENT_STATE) {
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            printf("[BLE] HCI ready, starting scan...\n");
            start_scan();
        }
        return;
    }

    if (event_type == GAP_EVENT_ADVERTISING_REPORT) {
        handle_advertising_report(packet);
        return;
    }

    if (event_type == HCI_EVENT_META_GAP &&
        hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
        uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            // 連線嘗試失敗（逾時、裝置已離開範圍等），不會再有 HCI_EVENT_
            // DISCONNECTION_COMPLETE 補上——沒有這個檢查的話狀態機會卡在
            // BLE_STATE_CONNECTING，永遠不會回去掃描。
            printf("[BLE] connection failed (status=0x%02x), resuming scan.\n", status);
            s_connection_handle = HCI_CON_HANDLE_INVALID;
            start_scan();
            return;
        }
        s_connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
        printf("[BLE] connected, handle=0x%04x\n", s_connection_handle);
        if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
            // 先配對，配對完成後才繼續探索/訂閱（見 proceed_after_pairing()）。
            // 額溫槍/血氧計不需要配對就能用同一套自訂 pipe，但血壓計這台實測
            // 需要先配對成功才能訂閱/寫入成功，繼續保留這個差異，沒有一起拿掉。
            s_ble_state = BLE_STATE_PAIRING;
            sm_request_pairing(s_connection_handle);
        } else if (s_handle_cache[s_current_kind].cached) {
            printf("[BLE] using cached handles for kind=%d, skipping discovery.\n", s_current_kind);
            s_fora_service = s_handle_cache[s_current_kind].service;
            s_fora_characteristic = s_handle_cache[s_current_kind].characteristic;
            enable_value_updates();
        } else {
            discover_service();
        }
        return;
    }

    if (event_type == HCI_EVENT_DISCONNECTION_COMPLETE) {
        uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
        printf("[BLE] disconnected (reason=0x%02x), resuming scan.\n", reason);
        s_connected_and_ready = false;
        s_connection_handle = HCI_CON_HANDLE_INVALID;
        start_scan();
        return;
    }
}

mode_ble_receive_exit_t mode_ble_receive_run(uint32_t idle_timeout_ms) {
    // 只是給畫面顯示用（個案編號），讀一次 flash 就好；如果從沒設定過
    // （storage_load_config() 回傳 false），display_status 那邊會顯示 "(unset)"。
    s_have_display_config = storage_load_config(&s_display_config);

    s_idle_timeout_ms = idle_timeout_ms;
    s_connected_and_ready = false;
    s_connection_handle = HCI_CON_HANDLE_INVALID;
    s_ble_state = BLE_STATE_IDLE;
    s_current_kind = FORA_DEVICE_UNKNOWN;
    // 注意：s_kind_cooldown_until[] 故意不在這裡重置——冷卻時間要跨越
    // BLE_RECEIVE/UPLOAD 模式切換持續有效，見上面宣告處的說明。
    s_got_any_reading_this_session = false;

    l2cap_init();
    gatt_client_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    // 血壓計連線後會先走一次配對（見 handle_hci_event() 連線完成分支）。故意不開
    // SM_AUTHREQ_BONDING——這裝置量測完就斷線、下次又是新的連線，如果請求
    // bonding，BTstack 會把這次配對產生的長期金鑰存起來，下次對同一個位址重
    // 連線時可能會嘗試用舊金鑰做「重新加密」而不是重新走一次配對流程；如果
    // 裝置那邊沒有真的把 bonding 資訊存起來（很多簡單的裝置韌體不支援），
    // 金鑰對不上，這個重新加密的請求就會卡住直到連線逾時，而不是乾脆地失敗。
    // 不開 bonding 的話每次都是全新的臨時金鑰，不會有這個「舊金鑰對不上」的問題。
    sm_set_authentication_requirements(0);

    s_hci_event_callback_registration.callback = &handle_hci_event;
    hci_add_event_handler(&s_hci_event_callback_registration);

    s_sm_event_callback_registration.callback = &handle_sm_event;
    sm_add_event_handler(&s_sm_event_callback_registration);

    hci_power_control(HCI_POWER_ON);

    s_last_reading_at = get_absolute_time();
    while (true) {
        led_status_poll();

        // KEY2 顯示歷史畫面期間暫停呼叫 display_status_poll()，不然畫面會
        // 馬上被 BLE_RECEIVE 即時內容蓋掉；逾時後恢復正常輪詢，poll() 會因為
        // s_ble_screen_is_current 已經被 show_upload_history() 清成 false
        // 而強制刷新一次，換回即時畫面。
        if (s_showing_history) {
            if (time_reached(s_history_view_until)) {
                s_showing_history = false;
            }
        } else {
            display_status_poll();
        }

        // 只有在 Scanning 狀態（沒有裝置連線中）才需要定期把時間戳往前推進——
        // 一旦開始連線/配對/探索，畫面內容本來就會因為狀態切換而變動，不需要
        // 額外靠這個計時器刷新。跟其他讀值時間戳一樣走 display_status_poll()
        // 的內容比對機制，只有時間戳字串真的變了才會觸發一次全刷，不會每輪
        // 迴圈都刷新面板。每 180 秒觸發一次。
        if (s_ble_state == BLE_STATE_SCANNING &&
            absolute_time_diff_us(s_last_scanning_status_update_at, get_absolute_time()) / 1000 >= 180 * 1000) {
            update_scanning_status();
        }

        // KEY0 長按：使用者手動要求進入熱點設定模式，不用重新插拔電源找
        // BOOTSEL。跟 BOOTSEL 那條路徑並存，沒有接這片電子紙的機器讀到的
        // 一律是「沒按下」，見 button_input.h 的說明。
        if (button_input_key0_long_press(KEY0_ENTER_CONFIG_HOLD_MS)) {
            printf("[BLE] KEY0 long-press detected, entering AP_CONFIG.\n");
            return MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG;
        }

        // KEY1：手動要求做一次完整的 WiFi 動作（連線→強制重新 NTP 校時→
        // 上傳，見 mode_upload.c），跳過 idle timeout 的等待。不像 idle
        // timeout 那條路徑要求待傳佇列非空——就算沒有資料要傳，也要能連線
        // 確認一次網路時間校得準不準。
        if (button_input_key1_pressed()) {
            printf("[BLE] KEY1 pressed, manually triggering WiFi action + NTP resync.\n");
            wall_clock_request_resync();
            return MODE_BLE_RECEIVE_EXIT_UPLOAD;
        }

        // KEY2：顯示已上傳歷史摘要畫面，看幾秒後自動換回即時畫面。
        if (button_input_key2_pressed()) {
            vital_record_t history[KEY2_HISTORY_DISPLAY_ROWS];
            size_t shown = storage_get_recent_upload_history(history, KEY2_HISTORY_DISPLAY_ROWS);
            size_t total = storage_get_upload_history_count();
            printf("[BLE] KEY2 pressed, showing upload history (%u/%u).\n", (unsigned)shown, (unsigned)total);
            display_status_show_upload_history(history, shown, total);
            s_showing_history = true;
            s_history_view_until = make_timeout_time_ms(KEY2_HISTORY_VIEW_MS);
        }

        // 還沒校時成功的話，不用等收到裝置讀值才有機會嘗試 NTP——每隔
        // NTP_UNSYNCED_RETRY_MS 就主動連一次 WiFi 重試，避免裝置一直收不到
        // 任何生理訊號時永遠沒有機會校時。校時成功後 wall_clock_is_synced()
        // 變 true，這個分支就不會再觸發。
        if (!wall_clock_is_synced() &&
            absolute_time_diff_us(s_last_ntp_retry_at, get_absolute_time()) / 1000 >= NTP_UNSYNCED_RETRY_MS) {
            printf("[BLE] wall clock still unsynced, triggering WiFi to retry NTP.\n");
            s_last_ntp_retry_at = get_absolute_time();
            return MODE_BLE_RECEIVE_EXIT_UPLOAD;
        }

        // 每次量測都是「連線->拿一筆資料->斷線」的短暫過程，不是持續連線接收，
        // 所以這個無新資料的判斷要看「距離上次拿到資料多久」，不能只在剛好
        // 連線中的那一刻才檢查。而且要等「這一輪至少收到過一筆」才開始算——
        // 像血壓計整個充放氣量測要 30-45 秒，如果一進 BLE_RECEIVE 模式就開始
        // 倒數，量測還沒做完就會被切去 UPLOAD、逼著斷線，白白錯過這筆資料。
        if (s_got_any_reading_this_session) {
            int64_t idle_ms = absolute_time_diff_us(s_last_reading_at, get_absolute_time()) / 1000;
            if (idle_ms >= (int64_t)s_idle_timeout_ms) {
                if (storage_pending_count() > 0) {
                    return MODE_BLE_RECEIVE_EXIT_UPLOAD;
                }
                // 收到的都是重複量測、被 storage_append_record() 判重擋掉，待傳
                // 佇列其實是空的——沒有東西要傳，不需要為了「切去 UPLOAD 確認看
                // 看」特地連一次 WiFi（見 PROJECT_PLAN.md 第 6.3 節重複上傳的
                // 討論）。重置這一輪的旗標，等下一筆真正的新讀值再重新倒數；
                // 不重置的話這個 if 每輪迴圈都會成立，等於忙迴圈。
                s_got_any_reading_this_session = false;
            }
        }

        sleep_ms(20);
    }
}
