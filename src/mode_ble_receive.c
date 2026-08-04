#include "mode_ble_receive.h"

#include "common.h"
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
#define DEVICE_RECONNECT_COOLDOWN_MS 60000
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

// 血壓計訂閱 Indicate 後裝置沒有主動推播（實測過，等到裝置自己斷線都沒有），
// 之前單獨試過「連線後直接 Read」也一律被拒絕（att_status=0x02），懷疑是要
// 「先訂閱 Indicate 才允許 Read」這種韌體邏輯（有些簡化過的裝置端 GATT
// 實作會把 Read 權限跟 CCCD 訂閱狀態綁在一起）。所以訂閱成功後，如果過了
// 這段時間還沒等到裝置推播，就主動補發一次 Read 試試看。
#define BP_READ_FALLBACK_DELAY_MS 1500
static btstack_timer_source_t s_bp_read_fallback_timer;

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void start_scan(void) {
    s_ble_state = BLE_STATE_SCANNING;
    led_status_set(LED_SLOW_BLINK);
    // scan_type=1 使用主動掃描（會送 SCAN_REQ 換 SCAN_RESPONSE）。很多裝置把
    // 裝置名稱放在 scan response 而非主要廣播封包，被動掃描(0)會看不到名稱。
    gap_set_scan_parameters(1, 0x0030, 0x0030);
    gap_start_scan();
}

static void discover_service(void) {
    s_ble_state = BLE_STATE_DISCOVER_SERVICE;
    if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
        // 血壓計走標準 Blood Pressure Service，16-bit UUID，跟其他兩種裝置
        // 自訂的 128-bit UUID pipe 是完全不同的服務。
        gatt_client_discover_primary_services_by_uuid16(
            handle_gatt_client_event, s_connection_handle, BLOOD_PRESSURE_SERVICE_UUID16);
    } else {
        gatt_client_discover_primary_services_by_uuid128(
            handle_gatt_client_event, s_connection_handle, FORA_SERVICE_UUID128);
    }
}

static void discover_characteristic(void) {
    s_ble_state = BLE_STATE_DISCOVER_CHARACTERISTIC;
    if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
        gatt_client_discover_characteristics_for_service_by_uuid16(
            handle_gatt_client_event, s_connection_handle, &s_fora_service,
            BLOOD_PRESSURE_MEASUREMENT_UUID16);
    } else {
        gatt_client_discover_characteristics_for_service_by_uuid128(
            handle_gatt_client_event, s_connection_handle, &s_fora_service, FORA_CHARACTERISTIC_UUID128);
    }
}

// 額溫槍/血氧計用 Notify、血壓計用標準的 Indicate——同一套訂閱機制，只差
// CCCD 要寫入哪個設定值，收到值都走同一個 GATT_EVENT_NOTIFICATION 事件
// （btstack 對 Notify/Indicate 統一用這個事件）。
static void enable_value_updates(uint16_t configuration) {
    s_ble_state = BLE_STATE_ENABLE_NOTIFY;
    gatt_client_listen_for_characteristic_value_updates(
        &s_notification_listener, handle_gatt_client_event, s_connection_handle, &s_fora_characteristic);
    gatt_client_write_client_characteristic_configuration(
        handle_gatt_client_event, s_connection_handle, &s_fora_characteristic, configuration);
}

// 見 BP_READ_FALLBACK_DELAY_MS 前面的說明：訂閱 Indicate 之後等一段時間都沒有
// 推播，就補一次 Read。這個計時器到期時如果裝置已經斷線/換到別的狀態就不做事
// （s_ble_state 不是 LISTENING 代表已經拿到資料斷線了，或整個連線都結束了）。
static void bp_read_fallback_timer_fired(btstack_timer_source_t *timer) {
    (void)timer;
    if (s_ble_state != BLE_STATE_LISTENING) {
        return;
    }
    printf("[BLE] no indication received, trying a direct read...\n");
    gatt_client_read_value_of_characteristic(handle_gatt_client_event, s_connection_handle, &s_fora_characteristic);
}

// 訂閱成功後，裝置不會自動推播，要主動寫入觸發指令才會回傳目前量到的數值
// （已用舊版可動的 MicroPython 實作確認）。用 write-without-response，
// 不需要等待 ATT 回應。只有額溫槍/血氧計走這個自訂 OEM pipe 才需要這個指令，
// 血壓計是標準 Blood Pressure Measurement，沒有這個 characteristic 可以寫。
static void send_trigger_command(void) {
    gatt_client_write_value_of_characteristic_without_response(
        s_connection_handle, s_fora_characteristic.value_handle,
        sizeof(FORA_TRIGGER_COMMAND), (uint8_t *)FORA_TRIGGER_COMMAND);
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
        // 手動格式化浮點數，避免依賴 newlib-nano 預設未啟用的 printf float 支援。
        int whole = (int)records[i].value;
        int frac = (int)((records[i].value - (float)whole) * 100.0f);
        if (frac < 0) {
            frac = -frac;
        }
        printf("[BLE] parsed reading: type=%d value=%d.%02d\n", records[i].type, whole, frac);
    }
    s_last_reading_at = get_absolute_time();
    s_got_any_reading_this_session = true;
    s_kind_cooldown_until[s_current_kind] = make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS);
    led_status_set(LED_HEARTBEAT);
    // 已經拿到資料了，血壓計的補讀計時器（如果還沒到期）就不需要了。
    btstack_run_loop_remove_timer(&s_bp_read_fallback_timer);
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
                if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
                    enable_value_updates(GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION);
                } else {
                    enable_value_updates(GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                }
            } else if (s_ble_state == BLE_STATE_ENABLE_NOTIFY) {
                s_ble_state = BLE_STATE_LISTENING;
                s_connected_and_ready = true;
                printf("[BLE] value updates enabled (att_status=0x%02x)\n", att_status);
                // 血壓計是標準 Blood Pressure Measurement，訂閱 Indicate 之後靜待裝置
                // 主動推播（很多血壓計韌體在收到訂閱指令當下就會推播最後一次量測結果，
                // 用來解決「連線前就已經量完」這種時序問題），不用像額溫槍/血氧計那樣
                // 額外寫入觸發指令（那是自訂 OEM pipe 才有的協定，標準特徵值沒有）。
                if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE) {
                    // 見 BP_READ_FALLBACK_DELAY_MS 前面的說明：等一段時間看裝置會不會
                    // 自己推播，沒有的話就補一次 Read。
                    btstack_run_loop_set_timer_handler(&s_bp_read_fallback_timer, bp_read_fallback_timer_fired);
                    btstack_run_loop_set_timer(&s_bp_read_fallback_timer, BP_READ_FALLBACK_DELAY_MS);
                    btstack_run_loop_add_timer(&s_bp_read_fallback_timer);
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
            process_reading_payload(value, value_len);
            break;
        }

        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
            // 見 BP_READ_FALLBACK_DELAY_MS 前面的說明，這是等不到推播才補發的 Read。
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
        enable_value_updates(GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION);
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
            // 先配對，配對完成後才繼續探索/訂閱 Indicate（見 proceed_after_pairing()）。
            s_ble_state = BLE_STATE_PAIRING;
            sm_request_pairing(s_connection_handle);
        } else if (s_handle_cache[s_current_kind].cached) {
            printf("[BLE] using cached handles for kind=%d, skipping discovery.\n", s_current_kind);
            s_fora_service = s_handle_cache[s_current_kind].service;
            s_fora_characteristic = s_handle_cache[s_current_kind].characteristic;
            enable_value_updates(GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
        } else {
            discover_service();
        }
        return;
    }

    if (event_type == HCI_EVENT_DISCONNECTION_COMPLETE) {
        uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
        printf("[BLE] disconnected (reason=0x%02x), resuming scan.\n", reason);
        // 斷線後如果補讀計時器還沒到期，要取消掉，不然到時候拿失效的 connection
        // handle 去發 Read，白做工。
        btstack_run_loop_remove_timer(&s_bp_read_fallback_timer);
        s_connected_and_ready = false;
        s_connection_handle = HCI_CON_HANDLE_INVALID;
        start_scan();
        return;
    }
}

bool mode_ble_receive_run(uint32_t idle_timeout_ms) {
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

        // 每次量測都是「連線->拿一筆資料->斷線」的短暫過程，不是持續連線接收，
        // 所以這個無新資料的判斷要看「距離上次拿到資料多久」，不能只在剛好
        // 連線中的那一刻才檢查。而且要等「這一輪至少收到過一筆」才開始算——
        // 像血壓計整個充放氣量測要 30-45 秒，如果一進 BLE_RECEIVE 模式就開始
        // 倒數，量測還沒做完就會被切去 UPLOAD、逼著斷線，白白錯過這筆資料。
        if (s_got_any_reading_this_session) {
            int64_t idle_ms = absolute_time_diff_us(s_last_reading_at, get_absolute_time()) / 1000;
            if (idle_ms >= (int64_t)s_idle_timeout_ms) {
                return true;
            }
        }

        sleep_ms(20);
    }
}
