#include "mode_ble_receive.h"

#include "common.h"
#include "display_status.h"
#include "fora_protocol.h"
#include "led_status.h"
#include "storage.h"

#include "btstack.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

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

// 每種裝置拿到讀值之後，多久內不要再重新連線同一種裝置——裝置量測完常常會
// 持續廣播好一陣子，如果一拿到資料就馬上又進到 UPLOAD、上傳完馬上又回到
// BLE_RECEIVE 重新掃描，這時候裝置多半都還醒著/還在廣播，會立刻被重新連線
// 再讀一次幾乎一樣的數值，變成裝置被反覆喚醒、storage 裡累積一堆重複紀錄。
// 這個冷卻時間**不是**每次進 BLE_RECEIVE 模式就重置，是跨越 BLE_RECEIVE/
// UPLOAD 模式切換持續有效的，才能真正讓裝置閒置下來、有機會走到它自己的
// 休眠邏輯。依裝置種類分開追蹤，額溫槍的冷卻不影響血氧計、反之亦然。
//
// 2026-08-05 從單一共用的 60 秒改成依裝置種類分開設定：實測發現 FORA IR42
// 額溫槍看起來是「只要通電就持續廣播」，不是量測完才廣播一段時間就停——60
// 秒冷卻一到，裝置多半都還在原地廣播，馬上又被連線讀一次，讀到的值有時候
// 還在緩慢漂移（例如同一顆額溫槍隔幾輪讀到 36.5 -> 36.8 -> 37.0 -> 37.1，不是
// 同一個量測值不變），看起來比較像是連續感應中，不是「使用者又量了一次」，
// 造成裝置被反覆喚醒、待傳資料被反覆覆蓋成幾乎沒有意義的新值，所以額溫槍
// 拉長到 1 分鐘。
//
// 2026-08-05 稍後：血壓計（FORA D40）原本維持 5 秒冷卻，但實測發現它量完一次
// 之後會持續廣播非常久（觀察到的重連週期是 5 秒冷卻+連線+上傳耗時，約
// 30-40 秒一次循環，遠遠超過額溫槍的問題），導致裝置一直被 Pico 重新喚醒、
// 沒有機會真正休眠。現在有 fora_protocol_decode_measured_key() 帶來的裝置端
// 量測時間戳可以正確判斷「是不是同一次量測」（見 storage.c 的
// storage_append_record()），已經不需要靠「短冷卻、盡快抓到新量測」這個手段
// 來確保正確性了，所以比照額溫槍拉長到 1 分鐘，減少不必要的重連。
//
// 2026-08-05 再調整：拉到 1 分鐘後實測發現裝置還是完全不會自己關機（量測完
// 超過 30 分鐘依然持續廣播、回應連線）。使用者提供關鍵線索：這台裝置在「傳送
// 舊記錄」模式下設計是 3 分鐘無活動就會自動關機——但 Pico 每 60 秒就重新連線
// 一次，很可能每次連線都把裝置自己的關機倒數計時器重置掉了，導致它永遠撐不到
// 3 分鐘這個門檻。拉長到 4 分鐘（240 秒），比裝置的 3 分鐘門檻多留 1 分鐘餘裕，
// 讓 Pico 真的有機會空出一段夠長的時間不去碰它，才有機會讓裝置自己觸發關機。
// 判重邏輯不依賴冷卻時間長短（見上面的說明），拉長不會犧牲正確性，只是「偵測
// 到真正新的一次量測」最慢會慢 4 分鐘。血氧計目前還沒觀察到同樣的持續廣播/
// 不關機問題，維持 5 秒——如果之後也出現，一樣可以放心比照拉長。
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

static void discover_service(void) {
    s_ble_state = BLE_STATE_DISCOVER_SERVICE;
    // 2026-08-05 更正：血壓計也是走跟額溫槍/血氧計相同的 Nordic LED/Button
    // Service 自訂 128-bit UUID pipe，不是標準 Blood Pressure Service（見
    // fora_protocol.h 開頭的說明），三種裝置現在都用同一套探索方式。
    gatt_client_discover_primary_services_by_uuid128(
        handle_gatt_client_event, s_connection_handle, FORA_SERVICE_UUID128);
}

static void discover_characteristic(void) {
    s_ble_state = BLE_STATE_DISCOVER_CHARACTERISTIC;
    gatt_client_discover_characteristics_for_service_by_uuid128(
        handle_gatt_client_event, s_connection_handle, &s_fora_service, FORA_CHARACTERISTIC_UUID128);
}

// 三種裝置現在都走同一套自訂 pipe 的 Notify 機制（血壓計 2026-08-05 之前
// 誤以為要用標準 Indicate，見 fora_protocol.h 的更正說明）。
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

// 額溫槍/血氧計用 Notify、血壓計用標準 Indicate 推播，兩種來源的 payload
// 都送進這裡解析、存起來、斷線——共用同一份邏輯。
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
        // 用四捨五入到小數點後 1 位，不是無條件捨去——之前這裡跟 upload_api.c
        // 修過的那個 bug 是同一種截斷誤差（36.8 印成 36.79），只是這裡是 debug
        // log 專用的另一份格式化邏輯，之前沒有一起修到。
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

// 已知的 FORA IR42 位址（前面幾次連線測試確認過），debug log 直接鎖定這顆裝置，
// 不再只靠裝置名稱判斷──manufacturer data 可能出現在沒有帶名稱的廣播封包裡。
static const bd_addr_t FORA_KNOWN_ADDR = { 0xC0, 0x26, 0xDA, 0x28, 0xB6, 0xE6 };

// 除錯用：把掃描到的每個裝置名稱/位址/manufacturer data 印出來，方便直接比對
// 「量測前」跟「量測後」廣播內容有沒有變化——用來驗證體溫數值是不是直接透過
// 廣播封包（不需要建立連線）發出來的，而不是只能靠 GATT indication。
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
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(packet, &s_fora_characteristic);
            break;

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t att_status = gatt_event_query_complete_get_att_status(packet);
            if (att_status != ATT_ERROR_SUCCESS) {
                printf("[BLE] query failed at state=%d, att_status=0x%02x, disconnecting.\n", s_ble_state, att_status);
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
            // 目前沒有任何裝置會主動觸發 Read（血壓計那個「等不到推播就補讀」的
            // 備案已經拿掉，見上面 GATT_EVENT_QUERY_COMPLETE／BLE_STATE_ENABLE_NOTIFY
            // 分支的說明），這個 case 保留著是給以後可能需要用 Read 的裝置
            // （例如血糖）共用同一套解析流程。
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
// Indicate 還是先做服務/特徵值探索。
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

bool mode_ble_receive_run(uint32_t idle_timeout_ms) {
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
        display_status_poll();

        // 只有在 Scanning 狀態（沒有裝置連線中）才需要定期把時間戳往前推進——
        // 一旦開始連線/配對/探索，畫面內容本來就會因為狀態切換而變動，不需要
        // 額外靠這個計時器刷新。跟其他讀值時間戳一樣走 display_status_poll()
        // 的內容比對機制，只有時間戳字串真的變了才會觸發一次全刷，不會每輪
        // 迴圈都刷新面板。
        //
        // 2026-08-05：從 60 秒改成 180 秒——這是唯一一個「定期、沒有實際新
        // 事件也會觸發」的刷新來源（其他畫面更新都是因為真的有新讀值/狀態
        // 改變/待傳筆數變化才觸發），比照 Waveshare 資料手冊建議的刷新間隔
        // 下限（見 PROJECT_PLAN.md 12.5.1 節），沒有理由讓這個心跳計時器成為
        // 唯一違反建議值的刷新來源。跟 12.5.1 節「180 秒建議值不嚴格遵守」的
        // 決定不衝突：那個決定是說「真的有新資料/狀態要顯示時不要因為還沒滿
        // 180 秒就延遲顯示」，這裡剛好相反——沒有新事件，純粹是為了讓使用者
        // 能分辨「裝置還活著」而定期刷新的心跳，本來就沒有「越快越好」的理由。
        if (s_ble_state == BLE_STATE_SCANNING &&
            absolute_time_diff_us(s_last_scanning_status_update_at, get_absolute_time()) / 1000 >= 180 * 1000) {
            update_scanning_status();
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
                    return true;
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
