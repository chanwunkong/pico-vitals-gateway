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
} ble_receive_state_t;

static btstack_packet_callback_registration_t s_hci_event_callback_registration;
static hci_con_handle_t s_connection_handle = HCI_CON_HANDLE_INVALID;
static gatt_client_service_t s_fora_service;
static gatt_client_characteristic_t s_fora_characteristic;
static gatt_client_notification_t s_notification_listener;

static ble_receive_state_t s_ble_state = BLE_STATE_IDLE;
static volatile bool s_connected_and_ready = false;
static absolute_time_t s_last_reading_at;
static uint32_t s_idle_timeout_ms;

// FORA IR42 量測完只會短暫連線一下就主動斷線，留給我們做完整
// 服務/特徵值探索的時間可能不夠。裝置的 GATT attribute table 在多次連線之間
// 是固定的，所以探索一次後就快取 handle，下次連線直接跳去訂閱，
// 省掉兩次 ATT 來回，盡量在裝置斷線前完成訂閱。
static bool s_handles_cached = false;

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
    gatt_client_discover_primary_services_by_uuid128(
        handle_gatt_client_event, s_connection_handle, FORA_SERVICE_UUID128);
}

static void discover_characteristic(void) {
    s_ble_state = BLE_STATE_DISCOVER_CHARACTERISTIC;
    gatt_client_discover_characteristics_for_service_by_uuid128(
        handle_gatt_client_event, s_connection_handle, &s_fora_service, FORA_CHARACTERISTIC_UUID128);
}

static void enable_notifications(void) {
    s_ble_state = BLE_STATE_ENABLE_NOTIFY;
    gatt_client_listen_for_characteristic_value_updates(
        &s_notification_listener, handle_gatt_client_event, s_connection_handle, &s_fora_characteristic);
    // 特徵值屬性是 Notify（已用舊版可動的 MicroPython 實作確認），不是 Indicate。
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

    if (!fora_protocol_matches_advertisement(adv_data, adv_len)) {
        return;
    }

    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);

    printf("[BLE] matched FORA device %s, connecting...\n", bd_addr_to_str(addr));

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
                printf("[BLE] query failed at state=%d, att_status=0x%02x\n", s_ble_state, att_status);
                break;
            }
            if (s_ble_state == BLE_STATE_DISCOVER_SERVICE) {
                printf("[BLE] service discovered, looking up characteristic...\n");
                discover_characteristic();
            } else if (s_ble_state == BLE_STATE_DISCOVER_CHARACTERISTIC) {
                printf("[BLE] characteristic discovered, enabling notifications...\n");
                s_handles_cached = true; // 下次連線可以直接跳過探索
                enable_notifications();
            } else if (s_ble_state == BLE_STATE_ENABLE_NOTIFY) {
                printf("[BLE] notifications enabled (att_status=0x%02x), sending trigger command...\n", att_status);
                s_ble_state = BLE_STATE_LISTENING;
                s_connected_and_ready = true;
                send_trigger_command();
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

            vital_record_t record;
            if (fora_protocol_parse_reading(value, value_len, &record)) {
                record.received_at_ms = to_ms_since_boot(get_absolute_time());
                record.status = UPLOAD_STATUS_PENDING;
                storage_append_record(&record);
                // 手動格式化浮點數，避免依賴 newlib-nano 預設未啟用的 printf float 支援。
                int whole = (int)record.value;
                int frac = (int)((record.value - (float)whole) * 100.0f);
                if (frac < 0) {
                    frac = -frac;
                }
                printf("[BLE] parsed reading: type=%d value=%d.%02d\n", record.type, whole, frac);
                s_last_reading_at = get_absolute_time();
                led_status_set(LED_HEARTBEAT);
                // 已經拿到這次量測的數值，主動斷線，回到掃描狀態等下一次量測。
                gap_disconnect(s_connection_handle);
            } else {
                printf("[BLE] fora_protocol_parse_reading() returned false (payload 格式不符預期).\n");
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
        if (s_handles_cached) {
            printf("[BLE] using cached service/characteristic handles, skipping discovery.\n");
            enable_notifications();
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
    s_idle_timeout_ms = idle_timeout_ms;
    s_connected_and_ready = false;
    s_connection_handle = HCI_CON_HANDLE_INVALID;
    s_ble_state = BLE_STATE_IDLE;

    l2cap_init();
    gatt_client_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    s_hci_event_callback_registration.callback = &handle_hci_event;
    hci_add_event_handler(&s_hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);

    s_last_reading_at = get_absolute_time();
    while (true) {
        led_status_poll();

        // 每次量測都是「連線->拿一筆資料->斷線」的短暫過程，不是持續連線接收，
        // 所以這個 60 秒無新資料的判斷要看「距離上次拿到資料多久」，
        // 不能只在剛好連線中的那一刻才檢查。
        int64_t idle_ms = absolute_time_diff_us(s_last_reading_at, get_absolute_time()) / 1000;
        if (idle_ms >= (int64_t)s_idle_timeout_ms) {
            return true;
        }

        sleep_ms(20);
    }
}
