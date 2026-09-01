#include "mode_ble_receive.h"

#include "button_input.h"
#include "common.h"
#include "display_status.h"
#include "fora_protocol.h"
#include "led_status.h"
#include "mode_upload.h"
#include "rightest_protocol.h"
#include "storage.h"
#include "wall_clock.h"

#include "btstack.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

// KEY0 需要連續按住這麼久才會觸發進入熱點設定模式（見 button_input.h），
// 避免不小心碰到就誤觸發。
#define KEY0_ENTER_CONFIG_HOLD_MS 3000

// KEY2 觸發未上傳（PENDING/FAILED）紀錄畫面（見 storage_pending_records_page()），
// 顯示這麼久沒有再按 KEY2 翻頁就自動換回 BLE_RECEIVE 即時畫面；期間每按一次
// KEY2 都會重新從這個時間量開始倒數。
#define KEY2_HISTORY_VIEW_MS 8000
#define KEY2_HISTORY_DISPLAY_ROWS 7

// KEY2 短按判定用的短/長分界（見 button_input_key2_pressed() 的說明），跟
// KEY0/KEY1 統一成同一個 3000ms，目前執行期間沒有對應的 KEY2 長按動作。
#define KEY2_LONG_PRESS_HOLD_MS 3000

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
    // Rightest GM700SB 專用：PCL 開通/查型號/查記錄總數/逐筆讀記錄/PCL 關閉
    // 這一整段流程共用這一個狀態，實際進度靠 s_rightest_session_state 分辨
    // （見該 enum 的說明），不是每個步驟都各自開一個 ble_receive_state_t——
    // GM700SB 的通訊模型（三個各自獨立的 characteristic：控制/通知/寫入）
    // 跟 FORA 系列（單一 characteristic 兼 write+notify）差太多，硬塞進既有
    // 的 DISCOVER_SERVICE/DISCOVER_CHARACTERISTIC/ENABLE_NOTIFY 狀態會讓那些
    // 狀態的意義變得模糊，所以另外開一個。
    BLE_STATE_RIGHTEST_SESSION,
} ble_receive_state_t;

static btstack_packet_callback_registration_t s_hci_event_callback_registration;
static btstack_packet_callback_registration_t s_sm_event_callback_registration;
static hci_con_handle_t s_connection_handle = HCI_CON_HANDLE_INVALID;
static gatt_client_service_t s_fora_service;
static gatt_client_characteristic_t s_fora_characteristic;
static gatt_client_notification_t s_notification_listener;

static ble_receive_state_t s_ble_state = BLE_STATE_IDLE;
// GM700SB 反應式配對成功後要從哪一步重試——原本唯一的觸發點是
// BLE_STATE_ENABLE_NOTIFY 訂閱失敗，2026-09-02 加了 BLE_STATE_DISCOVER_
// SERVICE 這個新的觸發點（見 GATT_EVENT_QUERY_COMPLETE 的說明），兩者重試
// 時要呼叫的函式不一樣，用這個變數記住觸發當下是哪一步，見 SM_EVENT_
// PAIRING_COMPLETE 的說明。
static ble_receive_state_t s_rightest_pairing_resume_state = BLE_STATE_IDLE;
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
//   血壓計：官方休眠門檻 3 分鐘（180 秒）→ 2026-08-26 使用者要求設 200 秒
//   （原本多留 1 分鐘餘裕、共 240 秒），比官方休眠門檻多留 20 秒，連續量
//   兩次的情境下能比原本更快收到第二筆，使用者已知悉這個取捨。
//   血氧計：不是固定值，見下面 OXIMETER_SETTLE_WINDOW_MS/
//   OXIMETER_RESAMPLE_INTERVAL_MS/OXIMETER_POST_SETTLE_COOLDOWN_MS 的說明，
//   這裡的表格項只是初始值（開機後、還沒開始一輪觀察 session 之前用得到），
//   實際冷卻時間由 handle_oximeter_reading() 動態覆寫。
static const uint32_t DEVICE_RECONNECT_COOLDOWN_MS[FORA_DEVICE_KIND_COUNT] = {
    [FORA_DEVICE_UNKNOWN] = 0,
    [FORA_DEVICE_THERMOMETER] = 60 * 1000,
    [FORA_DEVICE_OXIMETER] = 0,
    [FORA_DEVICE_BLOOD_PRESSURE] = 200 * 1000,
    // MD6 現在有正式的量測解析/儲存/上傳流程（見 fora_protocol.c），冷卻給
    // 短一點方便連續測不同試片；每次連線結束前還會多做一段「往回翻頁」把
    // 同一次測試 session 其餘項目也抓完（見下面 record_backfill_state_t 的
    // 說明），冷卻時間跟這段翻頁無關。
    [FORA_DEVICE_MD6] = 10 * 1000,
    // 2026-08-31 使用者實機測試後決定改長：GM700SB 的藍牙晶片是持續廣播的
    // （不像其他裝置量測完才有動靜，見 handle_advertising_report() 的說明），
    // 原本比照 MD6 抓 10 秒方便連續測試，但實機測試發現這代表 Gateway 幾乎
    // 不間斷地重複連線/喚醒裝置，使用者希望比照其他裝置「量測完才連線」的
    // 觀感，改成跟額溫槍一樣的 60 秒；同時見下面 HCI_EVENT_DISCONNECTION_
    // COMPLETE 的處理，這個冷卻時間現在不管連線結果如何（成功讀到新資料、
    // 確認沒有新資料、解析失敗、逾時斷線）都會套用，不是只有
    // finish_rightest_session() 那幾個「乾淨結束」的路徑才有。
    [FORA_DEVICE_RIGHTEST_GM700SB] = 60 * 1000,
};
static absolute_time_t s_kind_cooldown_until[FORA_DEVICE_KIND_COUNT];

// 血氧計是夾著手指持續量測的裝置，跟額溫槍/血壓計那種「量一次就結束」不同：
// 手指沒拿開的話裝置可能一直有新讀值。居家照護的量測慣例是「等數值穩定再
// 記錄」（FDA／臨床衛教一致建議等 30-60 秒讓讀數穩定，這裡取下限 30 秒），
// 不是把量測過程中每一次連線收到的值都當成正式讀值——用「30 秒觀察視窗，
// 視窗內只保留最新一筆候選、視窗到了才真的送出」來近似這個做法，見
// handle_oximeter_reading() 的說明。
#define OXIMETER_SETTLE_WINDOW_MS (30 * 1000)
// 觀察視窗還沒到之前，冷卻設短一點讓 Pico 很快再連一次抓下一筆候選值，逼近
// 「持續觀察直到視窗結束」的效果；抓太密集沒有意義（BLE 連線本身有開銷），
// 3 秒抓一次、30 秒視窗大概抓 10 次，足夠代表「數值穩定下來的最新結果」。
// 沒有實機量到裝置真實刷新頻率可以參考（試過「連線中不斷線、收到就立刻再
// 觸發」的量測方式，但那次測試沒有收到任何 BLE 事件，原因待查），3 秒是
// 憑經驗抓的合理值，之後如果有機會量到真實數據，可以再微調。
#define OXIMETER_RESAMPLE_INTERVAL_MS (3 * 1000)
// 觀察視窗結束、真的送出候選值之後的冷卻時間——代表這次量測（這個 session）
// 已經結束，不要緊接著又開始下一輪 30 秒觀察，跟額溫槍/血壓計的冷卻邏輯
// 精神一致（給裝置機會真的休眠/病患把手指移開），90 秒是目前的估計值。
#define OXIMETER_POST_SETTLE_COOLDOWN_MS (90 * 1000)

// 血氧計目前這一輪 30 秒觀察 session 的狀態：session 有沒有在進行中、什麼
// 時候開始的、目前收到的最新候選讀值是什麼。**故意不在 mode_ble_receive_run()
// 開頭重置**（跟 s_kind_cooldown_until[] 同樣的道理）：如果 session 進行到
// 一半、剛好被切去 UPLOAD 模式（見 mode_ble_receive_run() 主迴圈裡 idle 逾時
// 的判斷——現在只有真的送出候選值那一刻才會推進 idle 計時器，見
// handle_oximeter_reading()/commit_records() 的說明，所以 session 進行中不會
// 卡住其他裝置資料的上傳時效），回到 BLE_RECEIVE 後這個 session 要能接著算，
// 不能從頭重新倒數 30 秒。s_oximeter_session_start 用的是開機以來的單調時鐘
// （get_absolute_time()），跨越模式切換依然正確，不受切換期間電台關閉影響。
static bool s_oximeter_session_active = false;
static absolute_time_t s_oximeter_session_start;
static vital_record_t s_oximeter_latest_candidate[FORA_MAX_READINGS_PER_NOTIFICATION];
static size_t s_oximeter_latest_candidate_count = 0;

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

// 血壓計（D40）跟 MD6 每次連線抓到「目前這一筆」（index=0）記錄、正式
// commit 之後，趁裝置還沒斷線，繼續往回翻頁把同一次連線視窗內裝置回報的
// 其他記錄也抓完——2026-08-26 在 MD6 跟 D40 上都實機驗證過：cmd 0x2B 回應的
// byte[2]|byte[3]<<8、把 FORA_BP_CMD_GET_RECORD_PART_A/B 的 index 參數
// （塞進 p1/p2，之前固定填 0）改成非 0 值，兩個假設都成立（MD6 index=1 真的
// 翻到一筆日期時間跟 index=0 相同、但項目/數值不同的記錄，例如同一次測試的
// HCT；D40 往回翻頁抓到血壓/血糖混合的歷史記錄，全部正確解析），見
// PROJECT_PLAN.md 第 6.5 節。
//
// 每次連線固定從 index=1 開始翻到 min(裝置回報筆數, RECORD_BACKFILL_SAFETY_CAP)，
// 不做跨連線的 flash 持久化進度（那一層設計過但還沒實機驗證就先不上，見
// PROJECT_PLAN.md）——2026-08-26 使用者決定單次連線內把裝置回報的筆數全部
// 抓完，不要用一個很小的數字卡住正常抓取。RECORD_BACKFILL_SAFETY_CAP 只是
// 防止 cmd 0x2B 回應格式解析錯誤/裝置回傳異常值時的無窮迴圈保險，故意設得
// 遠大於官方文件寫的 MD6 最大容量（1000 組），正常情況下都會被裝置實際
// 回報的筆數，或連續兩筆解析失敗，提前結束，不會真的抓到這個上限。
#define RECORD_BACKFILL_SAFETY_CAP 1500
typedef enum {
    RECORD_BACKFILL_IDLE = 0,
    RECORD_BACKFILL_WAITING_COUNT,
    RECORD_BACKFILL_WAITING_PART_A,
    RECORD_BACKFILL_WAITING_PART_B,
} record_backfill_state_t;
static record_backfill_state_t s_record_backfill_state = RECORD_BACKFILL_IDLE;
// 這次連線打算翻頁抓到第幾個 index（不含，夾在 RECORD_BACKFILL_SAFETY_CAP
// 以內）、目前翻到第幾個 index、連續解析失敗次數。
static uint16_t s_record_backfill_target_count = 0;
static uint16_t s_record_backfill_next_index = 0;
static uint8_t s_record_backfill_consecutive_skips = 0;
static uint8_t s_record_backfill_record_part_a[4];

// 「上次同步到哪一筆」的定位點，在 process_reading_payload() 處理 index=0
// （最新那筆）時從 flash 載入一次，backfill 往回翻頁（index 1, 2, 3...）時
// 拿同一份來比對——不是每翻一頁就重新讀 flash，是連線開始時的那一份，直到
// 這次連線走完 backfill 都不變（新的定位點要等這次連線真的處理完最新那筆
// 之後才會存回 flash，見 process_reading_payload() 的說明，不能提早覆蓋，
// 不然 backfill 比對的對象會變成剛剛才存的「這次最新那筆」，永遠比不出
// 「哪裡是上次同步的邊界」）。裝置時鐘不可信任，這裡比對的是原始 8 bytes
// 記錄內容本身，不是解析出來的裝置時間戳，見 storage.h
// storage_get_backfill_anchor() 的說明。
static uint8_t s_backfill_sync_anchor[8];
static bool s_backfill_sync_anchor_valid = false;

// --- Rightest GM700SB 專用狀態（見 BLE_STATE_RIGHTEST_SESSION 的說明）---
//
// GM700SB 用 3 個各自獨立的 characteristic（FEE1 控制/FEE2 通知/FEE3
// 寫入），跟 FORA 系列共用 s_fora_service/s_fora_characteristic 的單一
// characteristic 模型不通用，另外開一組。**目前每次連線都重新做完整的
// service/characteristic 探索，沒有比照 s_handle_cache 做 handle 快取**——
// GM700SB 不像 FORA 那樣量測完就急著斷線，量測資料量測完仍會持續廣播一段
// 時間，多花一輪探索的時間成本可以接受，先求正確、不做這個最佳化，見
// PROJECT_PLAN.md。
static gatt_client_service_t s_rightest_service;
static gatt_client_characteristic_t s_rightest_char_pcl;    // FEE1
static gatt_client_characteristic_t s_rightest_char_notify; // FEE2
static gatt_client_characteristic_t s_rightest_char_write;  // FEE3
static bool s_rightest_char_pcl_found;
static bool s_rightest_char_notify_found;
static bool s_rightest_char_write_found;
static gatt_client_notification_t s_rightest_notification_listener;
static rightest_reassembly_t s_rightest_reassembly;

// 寫入用的靜態緩衝區——BTstack 的 gatt_client_write_value_of_characteristic()
// 需要 value 指標在呼叫當下有效，用區域變數（函式一返回就消失）不保險，
// 比照 fora_protocol.c 的 FORA_TRIGGER_COMMAND 用持久性的緩衝區，見
// send_rightest_pcl_mode()/send_rightest_command() 的說明。
static uint8_t s_rightest_pcl_value;
static uint8_t s_rightest_command_buffer[8]; // 目前最長的指令（讀記錄）是 5 bytes，8 留餘裕

// GM700SB 這一整段流程（開 PCL -> 等自動推播的 meter ID -> 查記錄總數/書籤
// -> 逐筆讀記錄 -> 關 PCL，型號查詢已拿掉，見下面 enum 的說明）都在
// BLE_STATE_RIGHTEST_SESSION
// 這一個 ble_receive_state_t 底下跑，靠這個子狀態機分辨目前進度、指揮下一步
// 要送什麼指令。狀態轉換完全由收到的 FEE2 Notify（重組完成後）推進，寫入
// FEE1/FEE3 的 ATT 回應（GATT_EVENT_QUERY_COMPLETE）刻意不處理、當作 no-op
// （見 handle_gatt_client_event() 裡 BLE_STATE_RIGHTEST_SESSION 的分支）——
// 寫入完成不代表裝置已經處理完、真正的下一步要等對應的 Notify 回來才知道。
typedef enum {
    RIGHTEST_SESSION_IDLE = 0,
    // 2026-08-28 實機測試：ATT 協定同一條連線一次只能有一個進行中的請求。
    // 訂閱完 Notify 先只等 meter-ID 自動推播（不主動送任何指令），推播到了
    // 才送 PCL 開啟，**PCL 開啟自己的 ATT 回應**（GATT_EVENT_QUERY_COMPLETE，
    // 不是等 Notify）到了才送型號查詢，見 RIGHTEST_SESSION_PCL_ON_PENDING。
    // 這裡故意只加這一個新狀態、範圍盡量小——加更多狀態、動到
    // finish_rightest_session() 的那個版本燒錄後整台裝置完全沒有任何序列埠
    // 輸出（疑似撞到 BTstack 內部斷言當機），已經還原，這次改用更保守的
    // 做法，見 PROJECT_PLAN.md 第 6.6 節。
    RIGHTEST_SESSION_WAIT_METER_ID,   // 剛訂閱完 Notify，等裝置自動推播一次 meter ID（內容忽略）
    RIGHTEST_SESSION_PCL_ON_PENDING,  // 已送 PCL 開啟指令，等它自己的 ATT 回應（不是等 Notify）；
                                       // 這段期間如果意外收到 Notify，直接忽略，不觸發任何寫入
                                       // （見 handle_rightest_notification() 的 case，不落入 default）
    // 2026-08-31 實機測試：拿掉型號查詢、改成 PCL 開啟 ATT 回應一到就立刻送
    // TYPE 1（index=0）之後，症狀原封不動地搬到了 TYPE 1 身上——ATT 層寫入
    // 一樣永遠成功確認，裝置一樣從沒回過任何 Notify，最後連線在 link layer
    // 逾時斷線（reason=0x08）。懷疑 ATT 層「寫入成功」只代表協定棧收到位元
    // 組，不保證裝置韌體的應用層已經真的處理完「切換進 PCL 模式」這個動作
    // （可能牽涉螢幕畫面切換、內部狀態機），加一段短暫延遲讓裝置有時間先
    // 穩定下來，再送 TYPE 1，見 RIGHTEST_PCL_SETTLE_MS。**這個延遲本身還沒
    // 實機驗證過是否真的是根因**，見 PROJECT_PLAN.md。
    RIGHTEST_SESSION_PCL_SETTLE,      // PCL 開啟 ATT 回應已到，等一段緩衝時間才送 TYPE 1；
                                       // 這段期間如果意外收到 Notify，比照 PCL_ON_PENDING 直接忽略
    RIGHTEST_SESSION_WAIT_SUMMARY,    // 已送 TYPE 1 查詢（index=0），等總筆數/書籤
    RIGHTEST_SESSION_WAIT_RECORD,     // 已送 TYPE 2 查詢，等這一筆記錄內容
} rightest_session_state_t;
// 2026-08-31：PCL 開啟 ATT 回應到了之後，不立刻送 TYPE 1，先等這麼久，見
// RIGHTEST_SESSION_PCL_SETTLE 的說明。憑經驗抓的值，沒有資料支撐，如果這次
// 測試發現還是收不到 Notify，可以先排除「純粹時序太快」這個假設。
#define RIGHTEST_PCL_SETTLE_MS 500
static rightest_session_state_t s_rightest_session_state = RIGHTEST_SESSION_IDLE;
static absolute_time_t s_rightest_pcl_settle_until;
static rightest_record_summary_t s_rightest_summary;
static uint16_t s_rightest_next_index;
static uint16_t s_rightest_target_count; // 這次連線打算讀到第幾個 index（含）

// 2026-09-01 曾經改成純手動觸發，2026-09-02 一度改成跟 FORA 系列一樣的
// 自動連線＋冷卻模式（見 git 歷史/PROJECT_PLAN.md §6.6），但實機測試發現
// GM700SB 連線嘗試本身會讓裝置發出提示音——FORA 系列裝置只有量測完那段
// 時間才廣播，自動連線自然稀疏；GM700SB 不管有沒有人在用都持續廣播，改成
// 自動連線等於每次冷卻一到就再嘗試一次、再響一次，病患會持續被打擾。
// **最終定案**：GM700SB 不做背景自動連線，只有兩種觸發來源會真的去比對/
// 連線，見 s_rightest_manual_sync_active 宣告處的說明：
//   1. KEY1 短按——護理人員臨時要用時手動觸發。
//   2. 每天固定時間自動觸發一次——見 RIGHTEST_AUTO_SYNC_HOUR，一天最多讓
//      裝置被連線嘗試/響一次，不是每次冷卻到期就響。
//
// KEY1_LONG_PRESS_HOLD_MS 同時是「短按觸發 GM700SB 同步」跟「長按觸發 WiFi/
// 上傳」（見 mode_ble_receive_run() 主迴圈）的分界門檻——放開時按住時間小於
// 這個值算短按，達到這個值算長按。2026-09-02 使用者決定跟 KEY0 進 AP_CONFIG
// 的長按門檻（KEY0_ENTER_CONFIG_HOLD_MS）統一成 3 秒，避免同一台裝置上不同
// 按鍵的「長按」判定標準不一致，容易誤觸或搞混。
#define KEY1_LONG_PRESS_HOLD_MS 3000
// 按了 KEY1 或每天定時觸發之後，最多等這麼久沒掃到 GM700SB 就自動放棄、
// 恢復忽略 GM700SB 廣播——避免視窗無限期留著，之後裝置隨便什麼時候出現
// 都會被自動連上、變相又回到全自動背景連線（就是要避免的提示音打擾）。
#define RIGHTEST_MANUAL_SYNC_WINDOW_MS (30 * 1000)
static bool s_rightest_manual_sync_active = false;
static absolute_time_t s_rightest_manual_sync_until;

// 每天固定時間自動打開一次跟 KEY1 短按完全相同的同步視窗（等同軟體幫使用者
// 按一次 KEY1），給居家照護情境用——個案端裝置留在家中跟 PICO 同一個空間，
// 不用等每週訪視護理人員來才手動同步，且一天只觸發一次，不會像背景自動
// 連線那樣持續讓裝置發出提示音。時間點使用者選定 22:00。用「距離上次校時
// 過的本地日期是否已經觸發過」判斷，不是固定間隔計時器，避免裝置中途重
// 開機/模式切換造成同一天觸發兩次以上——但重開機後這個判斷本身也會重置
// （下方宣告是一般 static，不落 flash），頂多造成同一天多觸發一次，不是
// 正確性問題。必須等 wall_clock_is_synced() 為 true 才會生效，沒校時成功
// 時 Pico 不知道現在是幾點，無從判斷（這段期間只有 KEY1 手動觸發能用）。
#define RIGHTEST_AUTO_SYNC_HOUR 22
static int64_t s_rightest_auto_sync_last_local_day = -1;

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
// 目前顯示第幾頁（0 起算）：s_showing_history 為 false 時（畫面已經換回
// BLE_RECEIVE 或是這次開機第一次按 KEY2）按下 KEY2 一律從第 0 頁重新開始；
// s_showing_history 已經是 true（使用者連續按著在翻頁）才會往下一頁推進，
// 見主迴圈 KEY2 分支的說明。
static size_t s_history_page = 0;

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

// Rightest GM700SB 專用：走 Service 0xFEE0，跟 FORA 系列的 128-bit UUID
// 自訂 pipe 完全不同，見 rightest_protocol.h 開頭的說明。
static void discover_rightest_service(void) {
    s_ble_state = BLE_STATE_DISCOVER_SERVICE;
    s_discovery_found = false;
    gatt_client_discover_primary_services_by_uuid16(
        handle_gatt_client_event, s_connection_handle, RIGHTEST_SERVICE_UUID16);
}

// 一次探索整個 service 底下全部的 characteristic（不像 FORA 用 UUID 指定
// 只找一個），逐一比對 uuid16 收集 FEE1/FEE2/FEE3 三個控制/通知/寫入
// characteristic，見 GATT_EVENT_CHARACTERISTIC_QUERY_RESULT 的處理。
static void discover_rightest_characteristics(void) {
    s_ble_state = BLE_STATE_DISCOVER_CHARACTERISTIC;
    s_discovery_found = false;
    s_rightest_char_pcl_found = false;
    s_rightest_char_notify_found = false;
    s_rightest_char_write_found = false;
    gatt_client_discover_characteristics_for_service(
        handle_gatt_client_event, s_connection_handle, &s_rightest_service);
}

static void enable_rightest_notify(void) {
    s_ble_state = BLE_STATE_ENABLE_NOTIFY;
    gatt_client_listen_for_characteristic_value_updates(
        &s_rightest_notification_listener, handle_gatt_client_event, s_connection_handle,
        &s_rightest_char_notify);
    gatt_client_write_client_characteristic_configuration(
        handle_gatt_client_event, s_connection_handle, &s_rightest_char_notify,
        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
}

// 寫 FEE1（PCL Mode 開關），mode 是 RIGHTEST_PCL_MODE_ON/OFF。這個
// characteristic 同時列出 Write/Write Without Response 兩種屬性（2026-08-28
// LightBlue 實機確認），但為了跟下面 send_rightest_command() 一致（FEE3
// 只有 Write，沒有 Write Without Response，必須用等 ATT 回應的寫入方式），
// 這裡也統一用等回應的版本，簡化程式碼、不用分兩套。回應本身（
// GATT_EVENT_QUERY_COMPLETE）刻意不處理，見 s_rightest_session_state 宣告處
// 的說明。
static void send_rightest_pcl_mode(uint8_t mode) {
    s_rightest_pcl_value = mode;
    gatt_client_write_value_of_characteristic(
        handle_gatt_client_event, s_connection_handle,
        s_rightest_char_pcl.value_handle, 1, &s_rightest_pcl_value);
}

// 組好一筆指令（見 rightest_protocol_build_command()）寫進 FEE3。**FEE3 只
// 支援 Write（沒有 Write Without Response，2026-08-28 LightBlue 實機確認），
// 一定要用這個等 ATT 回應的版本**，不能像 FORA 那樣用
// gatt_client_write_value_of_characteristic_without_response()。
static void send_rightest_command(uint8_t cmd, const uint8_t *data, uint8_t data_len) {
    size_t len = rightest_protocol_build_command(cmd, data, data_len, s_rightest_command_buffer);
    // 2026-08-28 除錯用：印出實際要寫進 FEE3 的原始 bytes、value_handle，
    // 跟 gatt_client_write_value_of_characteristic() 呼叫本身的回傳碼（不是
    // ATT 層的回應，是 BTstack 這一層「有沒有成功排進佇列」的回傳值）——
    // 型號查詢完全收不到回應，需要先確認寫入這一步本身有沒有問題。
    printf("[BLE] rightest: writing to FEE3 (handle=0x%04x, %u bytes):", s_rightest_char_write.value_handle,
           (unsigned)len);
    for (size_t i = 0; i < len; i++) {
        printf(" %02x", s_rightest_command_buffer[i]);
    }
    printf("\n");
    uint8_t rc = gatt_client_write_value_of_characteristic(
        handle_gatt_client_event, s_connection_handle,
        s_rightest_char_write.value_handle, (uint16_t)len, s_rightest_command_buffer);
    if (rc != ERROR_CODE_SUCCESS) {
        printf("[BLE] rightest: gatt_client_write_value_of_characteristic() rejected the write, rc=0x%02x\n", rc);
    }
}

// 訂閱成功後，裝置不會自動推播，要主動寫入觸發指令才會回傳目前量到的數值
// （已用舊版可動的 MicroPython 實作確認）。用 write-without-response，
// 不需要等待 ATT 回應。
static void send_trigger_command(void) {
    gatt_client_write_value_of_characteristic_without_response(
        s_connection_handle, s_fora_characteristic.value_handle,
        sizeof(FORA_TRIGGER_COMMAND), (uint8_t *)FORA_TRIGGER_COMMAND);
}

// 血壓計/MD6「問記錄」的兩段式交換第一/二段（見 fora_protocol.h 的協定
// 說明）：cmd 只會是 FORA_BP_CMD_GET_RECORD_PART_A 或 _PART_B，使用者編號
// 固定用 FORA_BP_USER_CURRENT。index=0 是文件記載的「目前這一筆」，正式
// 流程一律用 0；非 0 的 index 塞進 p1/p2（16-bit 小端，呼應 cmd 0x2B 回應
// 筆數也是 byte[2]|byte[3]<<8 這種編碼），p3 保留填 0——2026-08-26 實機驗證
// 過 index=1 真的能翻到跟 index=0 同一次測試 session、但不同項目的另一筆
// 記錄（見 record_backfill_state_t 的說明、PROJECT_PLAN.md 第 6.5 節）。
static void send_bp_get_record_part_at_index(uint8_t cmd, uint16_t index) {
    uint8_t command[8];
    uint8_t p1 = (uint8_t)(index & 0xFF);
    uint8_t p2 = (uint8_t)((index >> 8) & 0xFF);
    fora_protocol_build_command(cmd, p1, p2, 0x00, FORA_BP_USER_CURRENT, command);
    gatt_client_write_value_of_characteristic_without_response(
        s_connection_handle, s_fora_characteristic.value_handle, sizeof(command), command);
}

// 問裝置目前有幾筆記錄（cmd 0x2B），回應格式見 fora_protocol.h 的協定說明
// ——p1=使用者編號，沿用跟記錄查詢一樣的 FORA_BP_USER_CURRENT；byte[2]|
// byte[3]<<8 = 筆數這個假設已經實機驗證過（見 record_backfill_state_t 的
// 說明），但樣本數不多，數字大時是否還一致沒測過，所以 backfill 流程仍用
// RECORD_BACKFILL_SAFETY_CAP 夾住上限（純粹防止這個回應值異常時無窮迴圈，
// 不是拿來限制正常抓取，見上面宣告處的說明）。
static void send_bp_get_record_count(void) {
    uint8_t command[8];
    fora_protocol_build_command(FORA_BP_CMD_GET_RECORD_COUNT, FORA_BP_USER_CURRENT, 0x00, 0x00, 0x00, command);
    gatt_client_write_value_of_characteristic_without_response(
        s_connection_handle, s_fora_characteristic.value_handle, sizeof(command), command);
}

// 純粹方便看序列埠 log 對照用的可讀名稱，不是任何協定/儲存邏輯的一部分。
static const char *vital_type_debug_name(vital_type_t type) {
    switch (type) {
        case VITAL_TYPE_TEMPERATURE: return "TEMP";
        case VITAL_TYPE_SPO2:        return "SPO2";
        case VITAL_TYPE_PULSE_RATE:  return "PULSE";
        case VITAL_TYPE_SYSTOLIC:    return "SYS";
        case VITAL_TYPE_DIASTOLIC:   return "DIA";
        case VITAL_TYPE_GLUCOSE:     return "GLUCOSE";
        case VITAL_TYPE_HCT:         return "HCT";
        case VITAL_TYPE_KETONE:      return "KETONE";
        case VITAL_TYPE_UA:          return "UA";
        case VITAL_TYPE_CHOL:        return "CHOL";
        case VITAL_TYPE_HB:          return "HB";
        default:                     return "UNKNOWN";
    }
}

// 同上，量測情境（見 fora_protocol.h 的 fora_measurement_mode_t）；只有血糖/
// MD6 這幾種裝置會填有意義的值，其他裝置固定是 0，印出來也是 "GEN" 但沒有
// 實際意義，不影響判讀。
static const char *measurement_mode_debug_name(uint8_t mode) {
    switch ((fora_measurement_mode_t)mode) {
        case FORA_MEASUREMENT_MODE_AC: return "AC";
        case FORA_MEASUREMENT_MODE_PC: return "PC";
        default:                       return "GEN";
    }
}

// 把已經確定要採信的讀值真正存進待傳佇列——這是唯一會推進 idle 計時器
// （s_last_reading_at/s_got_any_reading_this_session）的地方。血氧計觀察
// session 進行中收到的候選讀值不會呼叫這個函式（見 handle_oximeter_reading()
// 的說明），只有視窗到了、真的採信某一筆的那一刻才會呼叫，這樣血氧計的手指
// 沒拿開、一直有動靜也不會讓 idle 計時器一直被重置、卡住其他裝置已經量好、
// 在待傳佇列裡等待上傳的資料。
// 2026-09-02：回傳值改成 bool——true 代表這批記錄全部成功存進待傳佇列，
// false 代表至少一筆被 storage_append_record() 拒絕（佇列滿了，見 storage.c
// MAX_PENDING_RECORDS 的說明）。GM700SB 讀取流程靠這個回傳值決定要不要推進
// 自己的同步定位點（見 handle_rightest_notification() RIGHTEST_SESSION_
// WAIT_RECORD 的說明）——只有真的存進佇列才算「同步過」，不然佇列剛好滿的
// 那一刻讀到的記錄會被永久跳過、再也不會重試。其餘呼叫端（額溫槍/血氧計/
// 血壓計/MD6）目前不檢查這個回傳值，維持原本行為不變。
static bool commit_records(const vital_record_t *records, size_t record_count) {
    uint64_t now_ms = to_ms_since_boot(get_absolute_time());
    bool all_queued = true;
    for (size_t i = 0; i < record_count; i++) {
        vital_record_t record = records[i];
        record.received_at_ms = now_ms;
        record.status = UPLOAD_STATUS_PENDING;
        if (!storage_append_record(&record)) {
            all_queued = false;
        }
        // 手動格式化浮點數，避免依賴 newlib-nano 預設未啟用的 printf float 支援；
        // 用四捨五入到小數點後 1 位，不是無條件捨去。
        int tenths = (int)(record.value * 10.0f + (record.value >= 0.0f ? 0.5f : -0.5f));
        int whole = tenths / 10;
        int frac = tenths % 10;
        if (frac < 0) {
            frac = -frac;
        }
        printf("[BLE] parsed reading: type=%s(%d) value=%d.%d mode=%s\n",
               vital_type_debug_name(record.type), record.type, whole, frac,
               measurement_mode_debug_name(record.measurement_mode));
    }
    s_last_reading_at = get_absolute_time();
    s_got_any_reading_this_session = true;
    led_status_set(LED_HEARTBEAT);
    return all_queued;
}

// 結束這次 GM700SB 連線（不管是正常讀完、身份核對失敗、還是解析出錯）：
// 一律先嘗試關閉 PCL 模式再斷線，避免裝置停留在鎖定畫面（見
// rightest_protocol.h FEE1 的說明）。這裡送出關閉指令後不等待它真的完成就
// 馬上呼叫 gap_disconnect()——2026-08-28 加這段時還沒實機驗證過，2026-09-02
// 已經重複驗證多次（每次都看到這個關閉寫入的 ATT 回應是 att_status=0x1f，
// 代表沒有正常收到「寫入成功」的確認，但螢幕實測正常顯示時間、不是卡在
// PCL 鎖定畫面，且後續每次重新連線裝置都正常回應），目前判斷這個指令本身
// 有送達、只是斷線時序讓我們這端看不到成功的 ATT 回應，不影響裝置實際解鎖
// ——先不改成等待確認，如果之後真的遇到裝置卡在 PCL 畫面再回頭處理。
static void finish_rightest_session(void) {
    s_rightest_session_state = RIGHTEST_SESSION_IDLE;
    s_kind_cooldown_until[FORA_DEVICE_RIGHTEST_GM700SB] =
        make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS[FORA_DEVICE_RIGHTEST_GM700SB]);
    // 這次手動觸發的同步視窗已經用掉了（不管結果是讀到新資料、確認沒有
    // 新資料、還是解析失敗），關掉閘門、恢復平常忽略 GM700SB 廣播，見
    // s_rightest_manual_sync_active 宣告處的說明。
    s_rightest_manual_sync_active = false;
    printf("[BLE] rightest: closing PCL mode and disconnecting...\n");
    send_rightest_pcl_mode(RIGHTEST_PCL_MODE_OFF);
    gap_disconnect(s_connection_handle);
}

// GM700SB 的 FEE2 Notify 都送進這裡：先餵進重組緩衝區（見
// rightest_reassembly_feed() 的說明），沒收完整就先返回等下一包；收完整
// 之後依 s_rightest_session_state 目前進度決定這筆內容代表什麼、下一步要
// 送什麼指令。整段流程（開 PCL -> 核對型號 -> 查總數/書籤 -> 逐筆讀記錄 ->
// 關 PCL）見 rightest_session_state_t 宣告處的說明。
static void handle_rightest_notification(const uint8_t *value, uint16_t value_len) {
    if (s_rightest_session_state == RIGHTEST_SESSION_WAIT_METER_ID) {
        // 2026-08-28 實機確認：這則自動推播的原始 bytes（例如 16 bytes 的
        // 裝置序號字串）完全沒有帶協定文件說的 2-byte 分包表頭，直接是內容
        // 本身，餵進 rightest_reassembly_feed() 會因為表頭格式對不上被誤判
        // 成「還在等分包」而丟棄，狀態機永遠不會被推進。反正內容本來就
        // 忽略，這裡完全跳過重組邏輯，收到任何 Notify 就當作「可以送 PCL
        // 開啟」的訊號——**送完這裡不送下一個指令，要等它自己的 ATT 回應**
        // （見 GATT_EVENT_QUERY_COMPLETE 的 RIGHTEST_SESSION_PCL_ON_PENDING
        // 分支）才送型號查詢，避免兩個 ATT 請求同時在途。
        printf("[BLE] rightest: got meter-ID push (%u bytes), enabling PCL mode...\n",
               (unsigned)value_len);
        s_rightest_session_state = RIGHTEST_SESSION_PCL_ON_PENDING;
        send_rightest_pcl_mode(RIGHTEST_PCL_MODE_ON);
        return;
    }

    if (!rightest_reassembly_feed(&s_rightest_reassembly, value, value_len)) {
        return; // 還在收剩下的分包，或這包對不上預期已經被丟棄重來，等下一輪
    }
    const uint8_t *frame = s_rightest_reassembly.buffer;
    size_t frame_len = s_rightest_reassembly.length;

    switch (s_rightest_session_state) {
        case RIGHTEST_SESSION_PCL_ON_PENDING:
            // 正常不該在等 PCL 開啟的 ATT 回應期間又收到 Notify——保守起見
            // 明確列出這個 case、單純忽略，**不呼叫 finish_rightest_session()
            // 或送出任何新指令**，避免在前一個寫入還沒確認完成時又送一個，
            // 見 rightest_session_state_t 宣告處的說明。
            printf("[BLE] rightest: unexpected notification while PCL-on pending, ignoring.\n");
            rightest_reassembly_reset(&s_rightest_reassembly);
            break;

        case RIGHTEST_SESSION_PCL_SETTLE:
            // 比照 PCL_ON_PENDING：這段緩衝等待期間正常不該收到 Notify，
            // 保守起見單純忽略，不呼叫 finish_rightest_session()、不觸發
            // 提前送出 TYPE 1（那個動作要等 mode_ble_receive_run() 主迴圈
            // 的 time_reached() 判斷成立才會做），見 RIGHTEST_PCL_SETTLE_MS
            // 的說明。
            printf("[BLE] rightest: unexpected notification during PCL settle delay, ignoring.\n");
            rightest_reassembly_reset(&s_rightest_reassembly);
            break;

        case RIGHTEST_SESSION_WAIT_SUMMARY: {
            bool parsed = rightest_protocol_parse_record_summary(frame, frame_len, &s_rightest_summary);
            rightest_reassembly_reset(&s_rightest_reassembly);
            if (!parsed) {
                printf("[BLE] rightest: failed to parse record summary, aborting.\n");
                finish_rightest_session();
                return;
            }
            printf("[BLE] rightest: total=%u max_capacity=%u last_transmission_index=%u\n",
                   s_rightest_summary.total_count, s_rightest_summary.max_capacity,
                   s_rightest_summary.last_transmission_index);
            // 2026-09-01 實機測試推翻了「last_transmission_index 是可信賴的
            // 跨連線同步書籤」這個從一開始就沒驗證過的假設（見
            // rightest_protocol.h RIGHTEST_CMD_READ_RECORD 的說明）：同一台
            // 裝置前後兩次連線分別讀到 total=2/last=1（1 筆待讀）跟
            // total=3/last=3（書籤直接追上新的總筆數，代表「沒有新資料」），
            // 但這整個對話過程中 Gateway 從來沒有成功送出過一次 TYPE 2 讀取、
            // 也就是那台裝置上一筆使用者實測的血糖值完全沒有被讀到就已經被
            // 判定成「同步過」，資料就這樣漏掉了。
            //
            // 2026-09-02 進一步發現：改成「每次都固定重讀 index=1..
            // total_count、交給 storage_append_record() 判重」這個第一版
            // 修法本身也有問題——storage_append_record() 的判重只跟「上一筆
            // 同類型/來源裝置」比對（見 storage.c 的說明），不是跟「已經上傳
            // 成功的歷史」比對；同一次連線內依序 commit 好幾筆不同時間點的
            // 記錄時，每一筆都會跟「上一筆剛 commit 的」不一樣，全部被當成
            // 新資料重新塞進待傳佇列——代表下次總筆數變多、再重讀 1..
            // total_count 時，前幾筆已經上傳成功的舊記錄會被原封不動再上傳
            // 一次，變成每次同步的筆數都是累加、重複上傳。改成 Gateway 自己
            // 記一份「同步到第幾個 index」的定位點（借用 MD6/D40 backfill
            // 用的同一個 storage_get_backfill_anchor()/storage_set_backfill_
            // anchor() flash 儲存格，用 source_kind=GM700SB 當 key，anchor
            // buffer 前 2 bytes 存 16-bit 小端 index，不是裝置的書籤、也不
            // 需要裝置端行為可信賴），每讀完一筆（不管解析成功還是被判定
            // QC/Hi-flag 跳過）就立刻更新這個定位點，下次連線只讀真正沒讀過
            // 的 index，不會重複上傳。
            uint8_t rightest_sync_anchor[8] = { 0 };
            storage_get_backfill_anchor((uint8_t)FORA_DEVICE_RIGHTEST_GM700SB, rightest_sync_anchor);
            uint16_t gateway_synced_index =
                (uint16_t)rightest_sync_anchor[0] | ((uint16_t)rightest_sync_anchor[1] << 8);
            printf("[BLE] rightest: gateway last synced index=%u\n", gateway_synced_index);
            if (s_rightest_summary.total_count == 0 ||
                gateway_synced_index >= s_rightest_summary.total_count) {
                printf("[BLE] rightest: no new records since last sync.\n");
                finish_rightest_session();
                return;
            }
            s_rightest_next_index = (uint16_t)(gateway_synced_index + 1);
            uint16_t remaining = (uint16_t)(s_rightest_summary.total_count - gateway_synced_index);
            uint16_t capped = remaining > RECORD_BACKFILL_SAFETY_CAP ? RECORD_BACKFILL_SAFETY_CAP : remaining;
            s_rightest_target_count = (uint16_t)(gateway_synced_index + capped);
            printf("[BLE] rightest: reading records index=%u..%u...\n",
                   s_rightest_next_index, s_rightest_target_count);
            s_rightest_session_state = RIGHTEST_SESSION_WAIT_RECORD;
            uint8_t index_bytes[2] = {
                (uint8_t)(s_rightest_next_index & 0xFF), (uint8_t)(s_rightest_next_index >> 8) };
            send_rightest_command(RIGHTEST_CMD_READ_RECORD, index_bytes, sizeof(index_bytes));
            break;
        }

        case RIGHTEST_SESSION_WAIT_RECORD: {
            vital_record_t record;
            bool parsed = rightest_protocol_parse_record(frame, frame_len, &record);
            rightest_reassembly_reset(&s_rightest_reassembly);
            // 這個 index 的同步定位點要不要前進，看這筆記錄有沒有真的「處理
            // 完」：解析失敗（checksum/Hi-flag/QC，不是病人數值）算處理完，
            // 以後都不用再理它；解析成功但 commit_records() 回傳 false（待傳
            // 佇列剛好滿了，見 storage.c MAX_PENDING_RECORDS 的說明）代表這筆
            // 資料還沒真的存進系統，**不能**前進，不然下次同步不會再重試、
            // 資料就永久漏掉了——2026-09-02 使用者要求要保證「藍牙收資料不會
            // 漏」才加這個判斷。
            bool handled = true;
            if (parsed) {
                handled = commit_records(&record, 1);
                if (!handled) {
                    printf("[BLE] rightest: index=%u parsed but pending queue is full, "
                           "not advancing sync anchor (will retry next sync).\n",
                           s_rightest_next_index);
                }
            } else {
                printf("[BLE] rightest: index=%u did not parse as a valid reading "
                       "(checksum/Hi-flag/QC), skipping.\n", s_rightest_next_index);
            }
            if (handled) {
                uint8_t rightest_sync_anchor[8] = { 0 };
                rightest_sync_anchor[0] = (uint8_t)(s_rightest_next_index & 0xFF);
                rightest_sync_anchor[1] = (uint8_t)(s_rightest_next_index >> 8);
                storage_set_backfill_anchor((uint8_t)FORA_DEVICE_RIGHTEST_GM700SB, rightest_sync_anchor);
            }
            s_rightest_next_index++;
            if (s_rightest_next_index > s_rightest_target_count) {
                printf("[BLE] rightest: finished reading records.\n");
                finish_rightest_session();
                return;
            }
            uint8_t index_bytes[2] = {
                (uint8_t)(s_rightest_next_index & 0xFF), (uint8_t)(s_rightest_next_index >> 8) };
            send_rightest_command(RIGHTEST_CMD_READ_RECORD, index_bytes, sizeof(index_bytes));
            break;
        }

        default:
            finish_rightest_session();
            break;
    }
}

// record_backfill_state_t 翻頁流程收到的回應：每翻到一筆能解析成功的記錄就
// 呼叫 commit_records() 正式存進待傳佇列（不同項目是不同 vital_type_t，就算
// device_measured_key 相同——同一次測試 session——storage.c 判重也不會互相
// 誤判，見 common.h device_measured_key 欄位的說明）。連續兩筆解析失敗（視為
// 已經到底）、或抓到裝置回報的筆數（夾在 RECORD_BACKFILL_SAFETY_CAP 內），
// 都會停手斷線。血壓計（D40）跟 MD6 共用這一套機制（都是 s_current_kind ==
// BLOOD_PRESSURE/MD6 才會觸發，見 process_reading_payload() 的說明），用
// s_current_kind 決定要用哪一種格式解析，不是寫死 MD6。
static void handle_record_backfill_notification(const uint8_t *value, uint16_t value_len) {
    switch (s_record_backfill_state) {
        case RECORD_BACKFILL_WAITING_COUNT: {
            uint16_t count_guess = value_len >= 4 ? (uint16_t)(value[2] | (value[3] << 8)) : 0;
            uint16_t capped = count_guess > RECORD_BACKFILL_SAFETY_CAP ? RECORD_BACKFILL_SAFETY_CAP : count_guess;
            printf("[BLE] kind=%d record count=%u, backfilling index=1..%u...\n",
                   s_current_kind, count_guess, capped == 0 ? 0 : (unsigned)(capped - 1));
            if (capped <= 1) {
                // 裝置說只有 1 筆（或查詢失敗回 0/1），index=0 剛剛已經拿過
                // 了，沒有更多要補的。
                s_record_backfill_state = RECORD_BACKFILL_IDLE;
                gap_disconnect(s_connection_handle);
                return;
            }
            s_record_backfill_target_count = capped;
            s_record_backfill_next_index = 1;
            s_record_backfill_consecutive_skips = 0;
            s_record_backfill_state = RECORD_BACKFILL_WAITING_PART_A;
            send_bp_get_record_part_at_index(FORA_BP_CMD_GET_RECORD_PART_A, s_record_backfill_next_index);
            break;
        }
        case RECORD_BACKFILL_WAITING_PART_A: {
            memcpy(s_record_backfill_record_part_a, &value[2], sizeof(s_record_backfill_record_part_a));
            s_record_backfill_state = RECORD_BACKFILL_WAITING_PART_B;
            send_bp_get_record_part_at_index(FORA_BP_CMD_GET_RECORD_PART_B, s_record_backfill_next_index);
            break;
        }
        case RECORD_BACKFILL_WAITING_PART_B: {
            uint8_t combined[8];
            memcpy(&combined[0], s_record_backfill_record_part_a, sizeof(s_record_backfill_record_part_a));
            memcpy(&combined[4], &value[2], 4);

            // 翻到「上次同步到哪」那一筆了：這筆跟更舊的之前都同步過，直接
            // 停手斷線，不用繼續往回翻、也不要把這筆重新存一次。
            if (s_backfill_sync_anchor_valid && memcmp(combined, s_backfill_sync_anchor, 8) == 0) {
                printf("[BLE] kind=%d index=%u matches last-synced anchor, stopping backfill "
                       "(older records already synced).\n", s_current_kind, s_record_backfill_next_index);
                s_record_backfill_state = RECORD_BACKFILL_IDLE;
                gap_disconnect(s_connection_handle);
                break;
            }

            vital_record_t records[FORA_MAX_READINGS_PER_NOTIFICATION];
            size_t count = fora_protocol_parse_reading(s_current_kind, combined, sizeof(combined), records);
            if (count > 0) {
                commit_records(records, count);
                s_record_backfill_consecutive_skips = 0;
            } else {
                printf("[BLE] kind=%d index=%u did not parse as a valid reading, skipping.\n",
                       s_current_kind, s_record_backfill_next_index);
                s_record_backfill_consecutive_skips++;
            }
            s_record_backfill_next_index++;
            if (s_record_backfill_consecutive_skips >= 2 ||
                s_record_backfill_next_index >= s_record_backfill_target_count) {
                s_record_backfill_state = RECORD_BACKFILL_IDLE;
                gap_disconnect(s_connection_handle);
            } else {
                s_record_backfill_state = RECORD_BACKFILL_WAITING_PART_A;
                send_bp_get_record_part_at_index(FORA_BP_CMD_GET_RECORD_PART_A, s_record_backfill_next_index);
            }
            break;
        }
        default:
            s_record_backfill_state = RECORD_BACKFILL_IDLE;
            gap_disconnect(s_connection_handle);
            break;
    }
}

// 血氧計是夾著手指持續量測的裝置，量到的每一筆通知不能直接當成正式讀值
// （見 OXIMETER_SETTLE_WINDOW_MS 宣告處的說明：居家照護慣例是等數值穩定
// 再記錄，不是把量測過程中的每一次讀值都記下來）。這裡用「30 秒觀察視窗，
// 視窗內只保留最新一筆候選、視窗到了才真的 commit」近似這個做法：
//   - 收到讀值時，如果這是這一輪 session 的第一筆，記下 session 開始時間；
//     不管是不是第一筆，都用這次的值覆蓋掉候選值（只留最新一筆）。
//   - 如果距離 session 開始已經超過 OXIMETER_SETTLE_WINDOW_MS，代表視窗到了，
//     把候選值真的 commit_records() 進待傳佇列，session 結束，冷卻時間設成
//     OXIMETER_POST_SETTLE_COOLDOWN_MS（這次量測告一段落，不要馬上又開始
//     下一輪 30 秒觀察）。
//   - 視窗還沒到的話，只更新候選值、不 commit，冷卻時間設成
//     OXIMETER_RESAMPLE_INTERVAL_MS，讓 Pico 很快再連一次抓下一筆候選。
static void handle_oximeter_reading(const vital_record_t *records, size_t record_count) {
    if (!s_oximeter_session_active) {
        s_oximeter_session_active = true;
        s_oximeter_session_start = get_absolute_time();
        printf("[BLE] oximeter reading session started, observing up to %us before committing.\n",
               (unsigned)(OXIMETER_SETTLE_WINDOW_MS / 1000));
    }
    memcpy(s_oximeter_latest_candidate, records, record_count * sizeof(vital_record_t));
    s_oximeter_latest_candidate_count = record_count;

    int64_t elapsed_ms = absolute_time_diff_us(s_oximeter_session_start, get_absolute_time()) / 1000;
    if (elapsed_ms >= OXIMETER_SETTLE_WINDOW_MS) {
        printf("[BLE] oximeter settle window elapsed (%lldms), committing latest reading.\n",
               (long long)elapsed_ms);
        commit_records(s_oximeter_latest_candidate, s_oximeter_latest_candidate_count);
        s_oximeter_session_active = false;
        s_kind_cooldown_until[FORA_DEVICE_OXIMETER] = make_timeout_time_ms(OXIMETER_POST_SETTLE_COOLDOWN_MS);
    } else {
        s_kind_cooldown_until[FORA_DEVICE_OXIMETER] = make_timeout_time_ms(OXIMETER_RESAMPLE_INTERVAL_MS);
    }
}

// 三種裝置的 Notify payload 都送進這裡解析——共用同一份解析邏輯，但血氧計
// 走觀察視窗（見 handle_oximeter_reading() 的說明），其他兩種裝置量到就直接
// commit。不管走哪條路，處理完都主動斷線，回到掃描狀態等下一次連線。
static void process_reading_payload(const uint8_t *value, uint16_t value_len) {
    vital_record_t records[FORA_MAX_READINGS_PER_NOTIFICATION];
    size_t record_count = fora_protocol_parse_reading(s_current_kind, value, value_len, records);
    if (record_count == 0) {
        printf("[BLE] fora_protocol_parse_reading() returned 0 (payload 格式不符預期).\n");
        return;
    }

    if (s_current_kind == FORA_DEVICE_OXIMETER) {
        handle_oximeter_reading(records, record_count);
    } else {
        bool backfillable = (s_current_kind == FORA_DEVICE_MD6 || s_current_kind == FORA_DEVICE_BLOOD_PRESSURE);

        // index=0（最新那筆）先跟「上次同步到哪」的定位點比對——原始 8 bytes
        // 完全相同就代表這筆（以及更舊的）之前都同步過了，裝置端沒有新資料，
        // 不用再存一次、也不用進 backfill 往回翻頁。這裡才是每次連線真正載入
        // 定位點的地方，backfill 往回翻頁時用的是同一份，不會每翻一頁重讀。
        if (backfillable) {
            s_backfill_sync_anchor_valid =
                storage_get_backfill_anchor((uint8_t)s_current_kind, s_backfill_sync_anchor);
        }
        bool already_synced = backfillable && s_backfill_sync_anchor_valid && value_len >= 8 &&
            memcmp(value, s_backfill_sync_anchor, 8) == 0;

        if (already_synced) {
            printf("[BLE] kind=%d index=0 matches last-synced anchor, nothing new this connection.\n",
                   s_current_kind);
            s_kind_cooldown_until[s_current_kind] = make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS[s_current_kind]);
            gap_disconnect(s_connection_handle);
            return;
        }

        commit_records(records, record_count);
        s_kind_cooldown_until[s_current_kind] = make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS[s_current_kind]);
        if (backfillable && s_record_backfill_state == RECORD_BACKFILL_IDLE) {
            // 這是新資料，把它記成新的「上次同步到哪」定位點——注意這裡存的是
            // s_backfill_sync_anchor_valid/s_backfill_sync_anchor 載入時的舊值
            // 已經不需要了，可以放心覆寫成新值；backfill 往回翻頁比對用的是
            // 呼叫 storage_get_backfill_anchor() 當下複製出來的那份，不會受
            // 這裡 storage_set_backfill_anchor() 寫回 flash 影響。
            if (value_len >= 8) {
                storage_set_backfill_anchor((uint8_t)s_current_kind, value);
            }
            // index=0 這筆已經正式存進待傳佇列了，先別斷線，繼續往回翻頁把
            // 這次連線視窗內裝置回報的其他記錄也抓完，見 record_backfill_state_t
            // 的說明。血壓計（D40）跟 MD6 共用同一套機制。
            printf("[BLE] index=0 record committed, backfilling remaining records...\n");
            s_record_backfill_state = RECORD_BACKFILL_WAITING_COUNT;
            send_bp_get_record_count();
            return;
        }
    }
    // 已經拿到這次連線的數值（不管是不是最終要採信的那一筆），主動斷線，
    // 回到掃描狀態等下一次連線。
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
    // GM700SB 廣播名稱是序號，比對不出關鍵字，只能用 Service UUID 0xFEE0
    // 粗篩（見 rightest_protocol.h 的說明），這裡只是為了 debug log 印得
    // 出來，不影響實際連線判斷（見 handle_advertising_report()）。
    bool is_rightest = rightest_protocol_matches_advertisement(adv_data, adv_len);
    if (!is_fora && !is_rightest) {
        return;
    }

    printf("[BLE scan] addr=%s rssi=%d name=\"%s\" mfg_data=", bd_addr_to_str(addr), rssi, name);
    if (mfg_data == NULL) {
        printf("(none)");
    } else {
        for (uint8_t i = 0; i < mfg_len; i++) {
            printf("%02x ", mfg_data[i]);
        }
    }
    if (is_rightest) {
        // 2026-08-31 除錯用：完整印出這個裝置的原始廣播封包（不是只挑幾個
        // 我們認得的欄位），用來比對「剛量完血糖」跟「閒置中」兩種狀態下的
        // 廣播內容是不是真的不一樣——如果不一樣（哪怕只是某個 flag 或
        // service data 欄位變化），就有機會不連線也能分辨有沒有新資料；如果
        // 完全一樣，就代表只能靠連線進去讀 total_count/last_transmission_
        // index 才知道，見跟使用者討論的說明。只對 GM700SB 開，避免洗版。
        printf(" raw_adv(%u bytes)=", (unsigned)adv_len);
        for (uint8_t i = 0; i < adv_len; i++) {
            printf("%02x", adv_data[i]);
        }
    }
    printf("\n");
}

static void handle_advertising_report(uint8_t *packet) {
    if (s_ble_state != BLE_STATE_SCANNING) {
        return;
    }

    const uint8_t *adv_data = gap_event_advertising_report_get_data(packet);
    uint8_t adv_len = gap_event_advertising_report_get_data_length(packet);

    debug_print_advertisement(packet, adv_data, adv_len);

    fora_device_kind_t kind = FORA_DEVICE_UNKNOWN;
    bool matched = fora_protocol_matches_advertisement(adv_data, adv_len, &kind);
    if (!matched && s_rightest_manual_sync_active &&
        rightest_protocol_matches_advertisement(adv_data, adv_len)) {
        // 只是初步粗篩（Service UUID 0xFEE0，廣播名稱是序號比對不出型號，
        // 見 rightest_protocol.h 的說明）。原本規劃連線配對後再送型號查詢
        // 二次核對身份，但 2026-08-28 實機測試發現裝置不會回應這個查詢
        // （ATT 層寫入永遠成功，但沒有 Notify 回應——回頭看協定文件附錄的
        // 資料同步流程圖，官方流程本來就不包含這一步），已經拿掉，目前
        // 這個 Service UUID 粗篩是唯一的身份確認機制，見
        // PROJECT_PLAN.md 第 6.6 節（誤配對到其他 0xFEE0 裝置的風險見那裡）。
        // GM700SB 廣播是持續性的，平常掃描到就直接忽略，只有
        // s_rightest_manual_sync_active 是 true 的這段期間（KEY1 短按或
        // 每天定時觸發）才會真的比對/連線——刻意不比照 FORA 系列掃到就連，
        // 因為連線嘗試本身會讓 GM700SB 發出提示音，背景持續自動連線會一直
        // 打擾病患，見上面宣告處的說明。
        kind = FORA_DEVICE_RIGHTEST_GM700SB;
        matched = true;
    }
    if (!matched) {
        return;
    }

    if (!time_reached(s_kind_cooldown_until[kind])) {
        return; // 這種裝置還在冷卻時間內，不要再打擾它
    }

    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);

    s_current_kind = kind;
    printf("[BLE] matched device %s (kind=%d), connecting...\n", bd_addr_to_str(addr), kind);

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
            if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
                gatt_event_service_query_result_get_service(packet, &s_rightest_service);
            } else {
                gatt_event_service_query_result_get_service(packet, &s_fora_service);
            }
            s_discovery_found = true;
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
                // 一次探索整個 service，不是像 FORA 那樣指定 UUID 只找一個，
                // 這裡會連續收到好幾個 characteristic，依 uuid16 分別收進
                // FEE1/FEE2/FEE3 三個欄位，見 discover_rightest_characteristics()
                // 的說明。
                gatt_client_characteristic_t characteristic;
                gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
                if (characteristic.uuid16 == RIGHTEST_CHARACTERISTIC_PCL_UUID16) {
                    s_rightest_char_pcl = characteristic;
                    s_rightest_char_pcl_found = true;
                } else if (characteristic.uuid16 == RIGHTEST_CHARACTERISTIC_NOTIFY_UUID16) {
                    s_rightest_char_notify = characteristic;
                    s_rightest_char_notify_found = true;
                } else if (characteristic.uuid16 == RIGHTEST_CHARACTERISTIC_WRITE_UUID16) {
                    s_rightest_char_write = characteristic;
                    s_rightest_char_write_found = true;
                }
            } else {
                gatt_event_characteristic_query_result_get_characteristic(packet, &s_fora_characteristic);
            }
            s_discovery_found = true;
            break;

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t att_status = gatt_event_query_complete_get_att_status(packet);
            bool is_discovery_state = s_ble_state == BLE_STATE_DISCOVER_SERVICE ||
                                       s_ble_state == BLE_STATE_DISCOVER_CHARACTERISTIC;
            if (att_status != ATT_ERROR_SUCCESS || (is_discovery_state && !s_discovery_found)) {
                // 2026-08-28 實機測試發現：GM700SB 對 FEE2 CCCD 寫入（開啟
                // Notify）直接回 ATT_ERROR_INSUFFICIENT_AUTHENTICATION
                // (0x05)，不是主動送 SM Security Request（先前那個猜測，見
                // handle_hci_event() 連線完成分支殘留的說明，已經證實走不通）
                // ——這其實是藍牙標準裡「中央端該主動配對」的訊號：收到這個
                // ATT 錯誤才呼叫 sm_request_pairing()，配對成功後重試剛剛
                // 失敗的那個操作。原本只在 BLE_STATE_ENABLE_NOTIFY（訂閱
                // FEE2）這一步實測會走到這裡，其他步驟（服務/characteristic
                // 探索）不需要加密就能查詢成功。
                //
                // 2026-09-02 擴大範圍：使用者清空 PICO 的 flash（連同 BTstack
                // 的配對金鑰）之後、GM700SB 那端還記得舊的配對紀錄，造成兩邊
                // 配對狀態不對稱——這次連最早的 BLE_STATE_DISCOVER_SERVICE
                // 都直接失敗，`att_status=0x1f`（不是標準 ATT 錯誤碼，懷疑是
                // 連線在查詢途中就被裝置那端中斷、BTstack 回報的內部狀態，
                // 見同一次斷線事件的 `reason=0x3e`）。**這個假設沒有把握**：
                // 先前實機測試過「PICO 主動在連線當下送配對請求」完全沒有
                // 收到裝置回應（見 PROJECT_PLAN.md 第 6.6 節 2026-08-28 第二次
                // 嘗試），跟這裡想做的事一樣是「主動要求配對」，那次沒有用。
                // 加這段是因為除了請使用者去翻血糖機選單清除配對之外沒有
                // 其他路可試，值得實機驗證一次；如果還是沒反應，代表這條路
                // 真的走不通，要嘛回頭找血糖機端清除配對的方法，要嘛這台
                // 裝置的配對記錄目前沒辦法在不動血糖機的情況下修好。
                if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB &&
                    (s_ble_state == BLE_STATE_ENABLE_NOTIFY ||
                     s_ble_state == BLE_STATE_DISCOVER_SERVICE) &&
                    (att_status == ATT_ERROR_INSUFFICIENT_AUTHENTICATION ||
                     att_status == ATT_ERROR_INSUFFICIENT_ENCRYPTION ||
                     att_status == 0x1f)) {
                    printf("[BLE] rightest: att_status=0x%02x at state=%d, "
                           "requesting pairing then retrying...\n", att_status, s_ble_state);
                    s_rightest_pairing_resume_state = s_ble_state;
                    s_ble_state = BLE_STATE_PAIRING;
                    sm_request_pairing(s_connection_handle);
                    break;
                }
                // 查詢本身可能「成功」完成卻完全沒有結果（陌生裝置沒有這個
                // service/characteristic），這種情況不能當成正常繼續往下走，
                // 不然會拿沒填過的 s_fora_service/s_fora_characteristic 去用。
                printf("[BLE] query failed or empty at state=%d, att_status=0x%02x, disconnecting.\n",
                       s_ble_state, att_status);
                gap_disconnect(s_connection_handle);
                break;
            }
            if (s_ble_state == BLE_STATE_DISCOVER_SERVICE) {
                if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
                    printf("[BLE] rightest: service 0xFEE0 discovered, looking up characteristics...\n");
                    discover_rightest_characteristics();
                } else {
                    printf("[BLE] service discovered, looking up characteristic...\n");
                    discover_characteristic();
                }
            } else if (s_ble_state == BLE_STATE_DISCOVER_CHARACTERISTIC) {
                if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
                    if (!s_rightest_char_pcl_found || !s_rightest_char_notify_found ||
                        !s_rightest_char_write_found) {
                        // 掃描階段只是靠 Service UUID 粗篩，這裡才發現 3 個
                        // characteristic 沒收齊，代表這台裝置其實不是
                        // GM700SB（或韌體版本跟這份協定文件對不上）——放棄，
                        // 不快取，見 rightest_protocol.h 的說明。
                        printf("[BLE] rightest: missing expected FEE1/FEE2/FEE3 characteristic "
                               "(pcl=%d notify=%d write=%d), disconnecting.\n",
                               s_rightest_char_pcl_found, s_rightest_char_notify_found,
                               s_rightest_char_write_found);
                        gap_disconnect(s_connection_handle);
                        break;
                    }
                    printf("[BLE] rightest: FEE1/FEE2/FEE3 all found, enabling notify...\n");
                    enable_rightest_notify();
                } else {
                    printf("[BLE] characteristic discovered...\n");
                    // 存進這種裝置專屬的快取，下次連上同種裝置可以直接跳過探索。
                    s_handle_cache[s_current_kind].cached = true;
                    s_handle_cache[s_current_kind].service = s_fora_service;
                    s_handle_cache[s_current_kind].characteristic = s_fora_characteristic;
                    enable_value_updates();
                }
            } else if (s_ble_state == BLE_STATE_RIGHTEST_SESSION) {
                // 大部分 FEE3 指令寫入的 ATT 回應刻意不處理——那幾步的下一步
                // 是靠收到的 FEE2 Notify 推進，不是靠寫入完成推進。只有 PCL
                // 開啟這個寫入例外：它自己的 ATT 回應才是「可以送型號查詢」
                // 的訊號（不是 Notify），見 rightest_session_state_t 宣告處
                // 的說明——同一條連線一次只能有一個在途的 ATT 請求，這裡如果
                // 也當 no-op，會跟型號查詢指令撞在一起送不出去。**這裡刻意
                // 不處理 PCL 關閉（finish_rightest_session()）的 ATT 回應，
                // 維持送出就直接斷線的舊寫法**——加了對應狀態的那個版本燒錄
                // 後整台裝置沒有任何序列埠輸出，已經還原，這次改用範圍更小
                // 的做法，只處理已經證實會卡住的這一個環節，見
                // PROJECT_PLAN.md 第 6.6 節。
                if (s_rightest_session_state == RIGHTEST_SESSION_PCL_ON_PENDING) {
                    // 2026-08-28 實機測試：型號查詢（0xB0 0x00）的寫入本身
                    // 在 ATT 層永遠成功確認，但裝置從沒回過任何 Notify——
                    // 回頭看協定文件附錄的「Measurement Data transmission
                    // Flow」流程圖，官方流程是 PCL 開啟後直接送「讀取總
                    // 筆數/書籤」（0xB0 0x61 0x00 0x00），中間完全沒有經過
                    // 型號查詢這一步（型號查詢是文件另一節單獨列出的指令
                    // 範例，不是資料同步流程的一部分）。改成照官方流程圖
                    // 走，跳過型號查詢；掃描階段的 Service UUID 0xFEE0 粗篩
                    // 是目前唯一的身份確認機制，沒有型號字串二次核對這道
                    // 保險，見 PROJECT_PLAN.md 第 6.6 節。
                    // 2026-08-31：不立刻送 TYPE 1，先進入 PCL_SETTLE 等一段
                    // 緩衝時間（見 RIGHTEST_PCL_SETTLE_MS 的說明），真正送出
                    // 的動作在 mode_ble_receive_run() 主迴圈的 time_reached()
                    // 判斷之後才觸發。
                    printf("[BLE] rightest: PCL mode enabled, waiting %ums before querying "
                           "record summary...\n", (unsigned)RIGHTEST_PCL_SETTLE_MS);
                    s_rightest_session_state = RIGHTEST_SESSION_PCL_SETTLE;
                    s_rightest_pcl_settle_until = make_timeout_time_ms(RIGHTEST_PCL_SETTLE_MS);
                } else {
                    // 2026-08-28 除錯用：型號查詢完全收不到回應，這裡印出來
                    // 確認「寫入這一步本身的 ATT 回應」到底有沒有收到——這個
                    // if 分支到得了，代表 att_status 已經是 SUCCESS（外層的
                    // att_status!=SUCCESS 檢查會在更前面攔截並印出/斷線）。
                    printf("[BLE] rightest: write acknowledged (session_state=%d), waiting for notify...\n",
                           s_rightest_session_state);
                }
                break;
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
                if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
                    // 接下來整段流程（開 PCL -> 核對型號 -> 查總數/書籤 ->
                    // 逐筆讀記錄 -> 關 PCL）都在 BLE_STATE_RIGHTEST_SESSION
                    // 底下跑，見該狀態、rightest_session_state_t 的說明。
                    // **先只等 meter-ID 自動推播，不在這裡送 PCL 開啟**——
                    // 同一條連線一次只能有一個在途的 ATT 請求，等推播到了
                    // 才送 PCL 開啟，見 handle_rightest_notification()。
                    s_ble_state = BLE_STATE_RIGHTEST_SESSION;
                    rightest_reassembly_reset(&s_rightest_reassembly);
                    s_rightest_session_state = RIGHTEST_SESSION_WAIT_METER_ID;
                } else if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE || s_current_kind == FORA_DEVICE_MD6) {
                    // 血壓計/MD6 走跟額溫槍/血氧計一樣的自訂 pipe，但要用「問
                    // 記錄」的兩段式交換取值（見 fora_protocol.h 的協定說明），
                    // 不是單一次觸發指令。這裡先送第一段，第二段在收到第一段
                    // 回應後才送（見 GATT_EVENT_NOTIFICATION 那邊的處理）。
                    s_bp_waiting_for_part_b = false;
                    // 每次新連線都重置翻頁狀態機——上次連線如果翻頁翻到一半
                    // 就被裝置斷線中斷，殘留的非 IDLE 值會讓這次連線的 index=0
                    // 回應被誤判成翻頁回應，見 record_backfill_state_t 的說明。
                    s_record_backfill_state = RECORD_BACKFILL_IDLE;
                    printf("[BLE] requesting latest record (part A)...\n");
                    send_bp_get_record_part_at_index(FORA_BP_CMD_GET_RECORD_PART_A, 0);
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

            if (s_ble_state == BLE_STATE_RIGHTEST_SESSION) {
                handle_rightest_notification(value, value_len);
                break;
            }

            if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE || s_current_kind == FORA_DEVICE_MD6) {
                if (value_len < 6 || value[0] != 0x51) {
                    printf("[BLE] unexpected BP/MD6 response, ignoring.\n");
                    break;
                }
                // 這個 if 分支本身已經限定 kind 是 BLOOD_PRESSURE 或 MD6（見
                // 外層條件），兩者共用同一套翻頁機制，不用再檢查 kind。
                if (s_record_backfill_state != RECORD_BACKFILL_IDLE) {
                    handle_record_backfill_notification(value, value_len);
                    break;
                }
                if (!s_bp_waiting_for_part_b) {
                    // 第一段回應：存起來，馬上送第二段——這兩段合起來才是完整
                    // 的一筆記錄，見 fora_protocol.h 的協定說明。
                    memcpy(s_bp_record_part_a, &value[2], sizeof(s_bp_record_part_a));
                    s_bp_waiting_for_part_b = true;
                    printf("[BLE] got record part A, requesting part B...\n");
                    send_bp_get_record_part_at_index(FORA_BP_CMD_GET_RECORD_PART_B, 0);
                } else {
                    // 第二段回應：跟第一段接成 8 bytes，交給共用的解析/儲存
                    // 邏輯——血壓計跟 MD6 現在都是正式流程，fora_protocol.c
                    // 的 fora_protocol_parse_reading() 依 kind 各自解析成
                    // 對應的 vital_record_t（見該檔案的說明）。
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

// 血壓計/MD6 連線後先配對，配對完成後才根據有沒有快取 handle，決定直接訂閱
// Notify 還是先做服務/特徵值探索。**GM700SB 不會呼叫這個函式**——它不是
// 我們主動配對觸發（見 handle_hci_event() 連線完成分支的說明），連線建立
// 當下就直接呼叫 discover_rightest_service()，不等配對；`SM_EVENT_
// PAIRING_COMPLETE` 對 GM700SB 是另外處理、不會呼叫到這裡（見
// handle_sm_event() 的說明），避免打斷已經在進行中的探索/讀寫流程。
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
                if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
                    // GM700SB 是收到 ATT 錯誤才反應式呼叫 sm_request_pairing()
                    // （見上面 GATT_EVENT_QUERY_COMPLETE 的說明），觸發點可能
                    // 是訂閱 FEE2 Notify 失敗、也可能是服務探索本身失敗
                    // （2026-09-02 新增，PICO 清空過配對金鑰、GM700SB 那端
                    // 還記得舊紀錄的情境），兩者重試要呼叫的函式不一樣，靠
                    // s_rightest_pairing_resume_state 記住的觸發點分流。
                    if (s_rightest_pairing_resume_state == BLE_STATE_DISCOVER_SERVICE) {
                        printf("[BLE] rightest: pairing complete, retrying service discovery...\n");
                        discover_rightest_service();
                    } else {
                        printf("[BLE] rightest: pairing complete, retrying notify subscription...\n");
                        enable_rightest_notify();
                    }
                } else {
                    printf("[BLE] pairing complete, proceeding...\n");
                    proceed_after_pairing();
                }
            } else {
                // 2026-08-28 修好：配對失敗原本完全沒有設定冷卻時間，斷線後
                // 立刻恢復掃描，如果失敗的裝置還在附近廣播，會立刻又重新
                // 掃到、立刻又重連、立刻又配對失敗，變成無限快速重試迴圈
                // ——GM700SB 實機測試時觸發、抓到的（血壓計/MD6 之前配對一
                // 直成功，沒機會暴露這個漏洞，但這幾種裝置共用同一段配對
                // 程式碼，同樣適用）。跟成功讀到資料後一樣，沿用
                // DEVICE_RECONNECT_COOLDOWN_MS 當冷卻時間，讓這段時間內
                // 別的裝置也有機會被掃到、連上，而不是被同一台一直配對
                // 失敗的裝置佔住。
                printf("[BLE] pairing failed (status=0x%02x), disconnecting.\n", status);
                s_kind_cooldown_until[s_current_kind] =
                    make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS[s_current_kind]);
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
        if (s_current_kind == FORA_DEVICE_BLOOD_PRESSURE || s_current_kind == FORA_DEVICE_MD6) {
            // 先配對，配對完成後才繼續探索/訂閱（見 proceed_after_pairing()）。
            // 額溫槍/血氧計不需要配對就能用同一套自訂 pipe，但血壓計這台實測
            // 需要先配對成功才能訂閱/寫入成功，繼續保留這個差異，沒有一起拿掉。
            // MD6 跟血壓計共用同一套底層實作（見 fora_protocol.h 的說明），
            // 還沒實機驗證過是否也需要配對，先假設需要——就算其實不需要，
            // 多走一次配對流程通常也不會出錯，比連不上、猜錯風險低。刻意不開
            // bonding（見 mode_ble_receive_run() 開頭的說明）——同一次開機
            // 期間如果先連過 GM700SB（見下面的分支，會把這個全域設定改成
            // SM_AUTHREQ_BONDING），這裡要主動改回來，不能留著上一台裝置的
            // 設定值。
            sm_set_authentication_requirements(0);
            s_ble_state = BLE_STATE_PAIRING;
            sm_request_pairing(s_connection_handle);
        } else if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
            // 2026-08-28 實機測試史：先試連線就主動 sm_request_pairing()，
            // GM700SB 完全不回應、等滿 30 秒逾時；改成完全不主動配對、直接
            // 探索，結果服務/characteristic 探索都不需要加密就成功，**訂閱
            // FEE2 Notify（寫 CCCD）才失敗，回傳 ATT_ERROR_INSUFFICIENT_
            // AUTHENTICATION**——這才是正確的訊號，代表 GM700SB 是用「直接
            // 拒絕操作」的標準 ATT 錯誤機制要求加密，不是主動送 SM Security
            // Request。改成收到那個 ATT 錯誤時才反應式呼叫
            // sm_request_pairing()（見 GATT_EVENT_QUERY_COMPLETE 的處理），
            // 這裡連線當下不主動配對，直接進探索。開 bonding（只影響
            // GM700SB 這條路徑，血壓計/MD6 維持原本不開 bonding，理由見
            // mode_ble_receive_run() 的說明）。**這一輪的重試邏輯還沒實機
            // 驗證過**，見 PROJECT_PLAN.md 第 6.6 節。
            sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
            discover_rightest_service();
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
        // 2026-08-31：GM700SB 逾時斷線（reason=0x08，讀取記錄卡住等到 link
        // layer 逾時）或連線建立失敗（探索/訂閱途中的其他斷線）原本完全沒有
        // 套用冷卻時間，只有 finish_rightest_session() 那幾個「乾淨結束」的
        // 路徑才會設。GM700SB 廣播是持續性的，斷線後立刻恢復掃描等於立刻又
        // 連上，不間斷地重試——實機測試連續 6 次都在這個狀態下連線後完全
        // 收不到 Notify、逾時斷線，懷疑高頻率重連沒有給裝置的藍牙協定棧喘息
        // /回收上次連線資源的時間。這裡不管斷線原因、也不管當下是不是真的在
        // 跑 rightest session，只要目前種類是 GM700SB 就套用同一個冷卻時間
        // （跟 finish_rightest_session() 重複設也沒關係，是 idempotent 操作）。
        if (s_current_kind == FORA_DEVICE_RIGHTEST_GM700SB) {
            s_kind_cooldown_until[FORA_DEVICE_RIGHTEST_GM700SB] =
                make_timeout_time_ms(DEVICE_RECONNECT_COOLDOWN_MS[FORA_DEVICE_RIGHTEST_GM700SB]);
            // 2026-09-01：逾時斷線走的是這條路，不會經過 finish_rightest_
            // session()，手動同步視窗也要在這裡一併關掉，不然逾時斷線之後
            // 閘門還開著、緊接著掃到的下一個廣播還是會自動連上去，等於沒有
            // 真的做到手動觸發，見 s_rightest_manual_sync_active 宣告處的
            // 說明。
            s_rightest_manual_sync_active = false;
        }
        // 2026-09-02 修好一個 bug：s_current_kind 是「這個模式期間最後一次
        // 成功匹配到的裝置種類」，不會在每次連線結束後自動重置，只有換一種
        // 別的裝置匹配到才會被覆蓋。之前只在 mode_ble_receive_run() 開頭重置
        // 一次，導致「這次斷線其實跟 GM700SB 無關（例如是別的 0xFEE0 裝置、
        // 或這次連線嘗試根本不是 GM700SB）」時，上面的判斷還是會被舊值誤觸發
        // ——實機測試觀察到，KEY1 剛開窗、緊接著一個完全無關的斷線事件就把
        // 剛開的視窗關掉，使用者等於白按。這裡統一重置，確保下一輪連線嘗試
        // 開始前 s_current_kind 一定是這次真正匹配到的種類，不會殘留舊值。
        s_current_kind = FORA_DEVICE_UNKNOWN;
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

        // GM700SB PCL 開啟的 ATT 回應到了之後，先緩衝 RIGHTEST_PCL_SETTLE_MS
        // 才送 TYPE 1（讀總筆數/書籤），見 rightest_session_state_t 宣告處
        // RIGHTEST_SESSION_PCL_SETTLE 的說明。放在主迴圈輪詢、不是收到 ATT
        // 回應當下就送，是為了不阻塞 BTstack 的事件處理（這裡不能直接
        // sleep_ms() 等待）。
        if (s_ble_state == BLE_STATE_RIGHTEST_SESSION &&
            s_rightest_session_state == RIGHTEST_SESSION_PCL_SETTLE &&
            time_reached(s_rightest_pcl_settle_until)) {
            printf("[BLE] rightest: settle delay elapsed, querying record summary...\n");
            s_rightest_session_state = RIGHTEST_SESSION_WAIT_SUMMARY;
            uint8_t index_zero[2] = { 0x00, 0x00 };
            send_rightest_command(RIGHTEST_CMD_READ_RECORD, index_zero, sizeof(index_zero));
        }

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

        // KEY1 短按：手動觸發 GM700SB 同步（見 s_rightest_manual_sync_active
        // 宣告處的說明——連線嘗試本身會讓 GM700SB 發出提示音，2026-09-02
        // 最終決定不做背景自動連線，只有這段視窗期間掃描到才會連）。
        if (button_input_key1_pressed(KEY1_LONG_PRESS_HOLD_MS)) {
            printf("[BLE] KEY1 pressed, listening for GM700SB for the next %us...\n",
                   (unsigned)(RIGHTEST_MANUAL_SYNC_WINDOW_MS / 1000));
            s_rightest_manual_sync_active = true;
            s_rightest_manual_sync_until = make_timeout_time_ms(RIGHTEST_MANUAL_SYNC_WINDOW_MS);
        }
        // 手動同步視窗逾時還沒掃到 GM700SB（裝置不在範圍內、或使用者按完
        // 忘記靠近）——自動關閉閘門，避免視窗無限期留著、之後裝置隨便什麼
        // 時候出現都會被自動連上，變相又回到全自動背景連線。真的連上的話
        // 閘門會在 finish_rightest_session()/逾時斷線處理提早關掉，這裡不會
        // 生效（s_rightest_manual_sync_active 那時已經是 false，time_reached()
        // 判斷再成立也沒差，不會有副作用）。
        if (s_rightest_manual_sync_active && time_reached(s_rightest_manual_sync_until)) {
            printf("[BLE] rightest: manual sync window expired without finding the device.\n");
            s_rightest_manual_sync_active = false;
        }

        // 每天 RIGHTEST_AUTO_SYNC_HOUR 點自動打開一次同步視窗，等同軟體幫
        // 使用者按一次 KEY1——只在還沒有視窗開著時才判斷，避免把使用者剛
        // 手動觸發的視窗覆蓋掉（覆蓋本身無害，但會讓 log 意圖不清楚）。
        if (wall_clock_is_synced() && !s_rightest_manual_sync_active) {
            uint64_t epoch_ms = wall_clock_to_epoch_ms(to_ms_since_boot(get_absolute_time()));
            int64_t local_sec = (int64_t)(epoch_ms / 1000) + LOCAL_UTC_OFFSET_SEC;
            int64_t local_day = local_sec / 86400;
            unsigned local_hour = (unsigned)((local_sec % 86400) / 3600);
            if (local_hour >= RIGHTEST_AUTO_SYNC_HOUR && s_rightest_auto_sync_last_local_day != local_day) {
                printf("[BLE] rightest: daily auto-sync window opening (local hour %u)...\n", local_hour);
                s_rightest_auto_sync_last_local_day = local_day;
                s_rightest_manual_sync_active = true;
                s_rightest_manual_sync_until = make_timeout_time_ms(RIGHTEST_MANUAL_SYNC_WINDOW_MS);
            }
        }

        // KEY1 長按：手動要求做一次完整的 WiFi 動作（連線→強制重新 NTP 校時→
        // 上傳，見 mode_upload.c），跳過 idle timeout 的等待。不像 idle
        // timeout 那條路徑要求待傳佇列非空——就算沒有資料要傳，也要能連線
        // 確認一次網路時間校得準不準。2026-09-02 改成長按（原本是短按，見
        // 上面 GM700SB 同步那段的說明）。
        if (button_input_key1_long_press(KEY1_LONG_PRESS_HOLD_MS)) {
            printf("[BLE] KEY1 long-press detected, manually triggering WiFi action + NTP resync.\n");
            wall_clock_request_resync();
            return MODE_BLE_RECEIVE_EXIT_UPLOAD;
        }

        // KEY2：顯示未上傳（PENDING/FAILED）紀錄，一次一頁。第一次按（或畫面
        // 已經逾時換回 BLE_RECEIVE 之後再按）固定從第 0 頁開始；畫面還顯示著
        // 的時候再按一次則翻到下一頁，翻完最後一頁繞回第 0 頁。
        if (button_input_key2_pressed(KEY2_LONG_PRESS_HOLD_MS)) {
            size_t total = storage_pending_count();
            size_t page_count = total == 0 ? 1 : (total + KEY2_HISTORY_DISPLAY_ROWS - 1) / KEY2_HISTORY_DISPLAY_ROWS;
            if (s_showing_history) {
                s_history_page = (s_history_page + 1) % page_count;
            } else {
                s_history_page = 0;
            }
            vital_record_t page_records[KEY2_HISTORY_DISPLAY_ROWS];
            size_t shown = storage_pending_records_page(page_records, KEY2_HISTORY_DISPLAY_ROWS,
                                                          s_history_page * KEY2_HISTORY_DISPLAY_ROWS);
            printf("[BLE] KEY2 pressed, showing pending records page %u/%u (%u shown, %u total).\n",
                   (unsigned)(s_history_page + 1), (unsigned)page_count, (unsigned)shown, (unsigned)total);
            display_status_show_pending_records(page_records, shown, total, s_history_page, page_count);
            s_showing_history = true;
            s_history_view_until = make_timeout_time_ms(KEY2_HISTORY_VIEW_MS);
        }

        // 還沒校時成功的話，不用等收到裝置讀值才有機會嘗試 NTP——每隔
        // NTP_UNSYNCED_RETRY_MS 就主動連一次 WiFi 重試，避免裝置一直收不到
        // 任何生理訊號時永遠沒有機會校時。校時成功後 wall_clock_is_synced()
        // 變 true，這個分支就不會再觸發。mode_upload_in_backoff() 為 true時
        // （WiFi 剛失敗過，見 mode_upload.c 的說明）先跳過、也不更新
        // s_last_ntp_retry_at，讓冷卻期一過就能立刻重試，不用再等一次完整的
        // NTP_UNSYNCED_RETRY_MS。
        if (!wall_clock_is_synced() && !mode_upload_in_backoff() &&
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
                    // WiFi 剛失敗過、還在冷卻期的話先不切去 UPLOAD（見
                    // mode_upload.c WIFI_FAILURE_BACKOFF_MS 的說明）——資料
                    // 已經安全存在待傳佇列的 flash 裡，不急著這次就傳，讓
                    // BLE_RECEIVE 能繼續掃描收資料，不用犧牲掃描時間去做一次
                    // 大概率還是會失敗的連線嘗試。故意不重置
                    // s_got_any_reading_this_session，冷卻期一過下一輪迴圈
                    // 就會立刻再檢查一次，不用等下一筆新讀值。
                    if (!mode_upload_in_backoff()) {
                        return MODE_BLE_RECEIVE_EXIT_UPLOAD;
                    }
                } else {
                    // 收到的都是重複量測、被 storage_append_record() 判重擋掉，
                    // 待傳佇列其實是空的——沒有東西要傳，不需要為了「切去
                    // UPLOAD 確認看看」特地連一次 WiFi（見 PROJECT_PLAN.md 第
                    // 6.3 節重複上傳的討論）。重置這一輪的旗標，等下一筆真正
                    // 的新讀值再重新倒數；不重置的話這個 if 每輪迴圈都會成立，
                    // 等於忙迴圈。
                    s_got_any_reading_this_session = false;
                }
            }
        }

        sleep_ms(20);
    }
}
