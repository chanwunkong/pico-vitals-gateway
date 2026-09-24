#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// 改編自 pico-sdk 官方測試專案 test/kitchen_sink/btstack_config.h（已驗證能跟
// CYW43 WiFi+BT combo chip 一起編譯）。本裝置只需要 BLE central（連線 FORA
// 週邊裝置），不使用 BLE peripheral、也不使用傳統藍牙(Classic)，但其餘的
// buffer/pool 大小巨集保留跟參考範例一致，避免拿掉後在 btstack_memory.c 等
// 別的檔案又冒出新的缺巨集編譯錯誤。

#define ENABLE_LE_CENTRAL
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVRCP_CONNECTIONS 2
#define MAX_NR_BNEP_CHANNELS 1
#define MAX_NR_BNEP_SERVICES 1
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES  2
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_HFP_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS  4
#define MAX_NR_L2CAP_SERVICES  3
#define MAX_NR_RFCOMM_CHANNELS 1
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 1
#define MAX_NR_SERVICE_RECORD_ITEMS 4
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1

// 2026-09-23 從 4 調到 16：只有 GM700SB 這條路徑會用到 bonding（見
// mode_ble_receive.c），4 是照抄 pico-sdk kitchen_sink 範例的示範值，不是
// 硬體限制，調大只多佔每筆 LTK/IRK 的固定大小記憶體，讓同一台 gateway
// 一輩子輪流配對過的血糖機數量不容易撞到上限（真正無上限需要改
// BTstack le_device_db_tlv.c 本身的固定筆數設計，這裡先抓一個實務上
// 幾乎不可能用完的數字）。
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// Limit number of ACL/SCO Buffer to use by stack to avoid cyw43 shared bus overrun
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link Key DB and LE Device DB using TLV on top of Flash Sector interface
// 2026-09-23：跟上面 MAX_NR_LE_DEVICE_DB_ENTRIES 的說明一樣，這是實際落地到
// flash TLV 的筆數，要跟著一起調大，不然記憶體裡的 DB 撐得住但 flash 端存
// 不下第 5 筆以後的配對金鑰。
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 4

// 沒有給 btstack malloc，用固定大小的 ATT DB。
#define MAX_ATT_DB_SIZE 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT

#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif // BTSTACK_CONFIG_H
