# 韌體檔案說明

> 這是逐檔案的程式碼導覽。**裝置支援的三種 FORA 裝置協定細節、目前整體狀態、測試交接清單，請看 [PROJECT_PLAN.md](PROJECT_PLAN.md)**，這份文件只講「哪個檔案負責什麼」。

主要功能一句話：**Pico W 用藍牙(BLE)當 central 連上生理量測裝置（目前支援 FORA IR42 額溫槍、FORA O2 血氧計、FORA D40 血壓計），收到數值後，切換成 WiFi，把資料透過 HTTPS POST 送到遠端 API**，中間穿插一個「熱點設定模式」讓使用者用手機設定 WiFi 帳密跟個案資訊。

## 燒錄觀念澄清

**不是每個檔案分別燒錄**。CMakeLists.txt 列出的所有 `.c` 檔案（連同它們各自 `#include` 的 `.h`）會被 ARM GCC 編譯成 `.o`，再由連結器合併成單一個 `pico_gateway.elf`，最後轉成 `build/pico_gateway.uf2`——**這一個 .uf2 檔才是真正被複製到 RPI-RP2 磁碟、燒進 Pico flash 的東西**。下面逐檔說明的是「這個 .uf2 是由哪些原始碼組成、各自負責什麼」。

除了我們自己寫的 `src/*.c`，最終的 .uf2 裡還包含 pico-sdk（BTstack 藍牙協定棧、lwIP 網路協定棧、mbedtls TLS、cyw43 WiFi/藍牙晶片驅動）的大量程式碼，這些不在這份文件列出（在 `C:\Users\WunKong\pico-sdk`），只列我們專案自己的檔案。

## 整體流程

```
開機
  │
  ▼
[mode_boot_select] 4 秒視窗偵測 BOOTSEL 是否按住
  ├─ 按住 → [mode_ap_config]  設定 WiFi/個案資訊，存進 flash
  └─ 沒按 → [mode_ble_receive] 掃描並連線 FORA 裝置，收到數值寫入 storage
                  │ 距離上次收到數值 60 秒沒有新訊號
                  ▼
             [mode_upload] 開 WiFi，把 storage 裡待傳資料 POST 到伺服器
                  │ 不論成功失敗
                  ▼
             回到 [mode_ble_receive]
```

這個切換邏輯統一寫在 `state_machine.c`，各模式的實作互相看不到彼此，只透過 `state_machine.c` 串起來。

---

## 進入點與流程控制

### `src/main.c`
程式的 `main()`。開機序列：初始化 USB 序列埠（`stdio_init_all()`，故意 `sleep_ms(1500)` 等 USB CDC 列舉完成，不然最早幾行 log 會被吃掉）→ 初始化 WiFi/藍牙晶片（`cyw43_arch_init()`，失敗就閃退卡死，因為之後任何燈號/無線功能都需要它）→ 初始化 LED 狀態機、flash 儲存 → 呼叫 `state_machine_run()`（這個函式是無窮迴圈，不會返回）。

### `src/state_machine.c` / `state_machine.h`
整個裝置的主控迴圈：`state_machine_run()` 先呼叫 `mode_boot_select_check()` 決定第一個狀態，之後在 `STATE_AP_CONFIG`／`STATE_BLE_RECEIVE`／`STATE_UPLOAD` 三個狀態間切換（見上面的流程圖）。**同一時間只允許一種無線模式運作**（WiFi 或藍牙擇一），這個規則靠 `radio_switch_to_wifi()` / `radio_switch_to_bluetooth()` 兩個函式統一把關，避免以後改個別模式時不小心兩個無線一起開（PROJECT_PLAN.md 第 2.1 節有解釋為什麼要這樣設計：RAM 有限、WiFi+BLE 同開的穩定度在社群案例裡不夠多）。**（2026-08-06 新增）** `STATE_BLE_RECEIVE` 離開時依 `mode_ble_receive_run()` 回傳的 `mode_ble_receive_exit_t` 決定去向：`MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG`（KEY0 長按）去 `STATE_AP_CONFIG`，其餘（idle timeout 或 KEY1 手動觸發）一律去 `STATE_UPLOAD`，跟原本只有「上傳」這一條路徑的行為相容。

---

## 開機模式選擇

### `src/mode_boot_select.c` / `mode_boot_select.h`
Pico W 沒有使用者按鍵，只有 BOOTSEL。這個檔案用一個取自官方範例的技巧（暫時把 QSPI_SS 腳位切成輸入、讀電位、再切回去）在**開機後、韌體正常執行中**偵測 BOOTSEL 有沒有被按著——注意這跟「開機時按著 BOOTSEL 進 USB 燒錄模式」是兩回事，那是晶片 boot ROM 的行為，跟這裡讀 GPIO 的程式碼完全無關，這裡的偵測是韌體已經開始跑之後才做的。`mode_boot_select_check(window_ms)` 在指定的毫秒數視窗內（目前 4 秒，LED 快閃提示），按住就回傳 `true`（進熱點設定模式），沒按就回傳 `false`（進 BLE 接收模式，也是預設的 24/7 常駐狀態）。

---

## 三個運作模式

### `src/mode_ap_config.c` / `mode_ap_config.h`
「熱點設定模式」。開一個獨立的 WiFi 熱點（SSID 每台裝置唯一衍生、密碼固定，見下方說明），搭配 `dhcpserver.c`／`dnsserver.c` 讓手機連上後自動配到 IP，並且不管查什麼網域都導回 Pico 自己觸發作業系統的 captive portal 自動彈出登入頁（模仿公用 WiFi 的行為，使用者不用自己開瀏覽器打網址）。進入這個模式時會先 `scan_nearby_wifi()` 掃描附近 WiFi，設定頁的 SSID 欄位是下拉選單（掃到的網路）+ 一個手動輸入欄位（給掃不到的隱藏網路用）。用 lwIP 的 raw TCP API 手刻一個極簡單的單連線 HTTP server（不是官方的 lwIP httpd），GET 回傳表單、POST 收表單資料後用 `storage_save_config()` 寫進 flash，然後結束（回到 BLE 接收模式）。

表單會先讀目前已存的設定（`storage_load_config()`）帶入頁面：SSID 下拉選單自動選中目前的網路、個案姓名/編號/個管師資訊/上傳伺服器網址會帶入現有值，**WiFi 密碼、上傳認證金鑰欄位留空 = 不變更目前值**（不會被清空覆蓋），方便使用者只改其中一項而不用重填全部欄位。

**（2026-08-06，實機測試後調整）** `generate_ap_ssid()`：SSID 前綴從 `"PicoGateway-Setup-"` 縮短成 `"GATEWAY-"`（配合電子紙螢幕顯示空間，見 `display_status.c` 說明），字尾仍是 CYW43 出廠 MAC 後 2 bytes 衍生的 4 位十六進位、每台裝置唯一。`generate_ap_password()`：**密碼改成所有裝置固定同一組 `02750963`**（原本是從 RP2040 board ID 衍生的每台裝置唯一值，但無螢幕版本沒有任何管道能讓使用者知道衍生出來的密碼是什麼，見 PROJECT_PLAN.md 第 8 節第 39 點）；`pico/unique_id.h` 相關 include／`CMakeLists.txt` 的 `pico_unique_id` 連結都已移除。表單也新增 `api_host`／`api_key` 兩個欄位，對應 `device_config_t` 新增的 `upload_server_host`／`upload_api_key`，留空的行為跟密碼欄位一致（見下方 `common.h`／`upload_api.c` 說明）。

**（2026-08-06，實機測試後復原）** 曾經試過拿掉 WiFi 下拉選單裡的訊號強度（dBm）顯示，並加一段簡約手機版 CSS（`<style>`）+ 把欄位標籤改成 `<label>` 結構。CSS 裡的 `width:100%` 沒有跳脫成 `%%`（`html_append()` 內部是 `vsnprintf()`，字串會被當成格式化字串解析），導致設定網頁一度打不開；修好這個問題後，實機測試仍發現手機出現「無法連上網路」提示（先前版本沒有），為了不在測試現場冒風險，**最終整個復原回加 CSS 之前的版面**，只保留「拿掉 dBm 顯示」這一項改動，見 PROJECT_PLAN.md 第 8 節第 40 點。

**這個模式跟「Pico 實際運作時要連的 WiFi」是兩個獨立網路**：這個熱點只是拿來讓你在設定介面上「填入」目標 WiFi 的帳密，Pico 存好設定後就會關掉這個熱點、改用你填的帳密去連目標 WiFi。

**（2026-08-06 加做，尚未實機測試）** `mode_ap_config_run()` 主迴圈原本只會等 `s_config_submitted`（網頁表單送出），現在加了 `button_input_key0_long_press(AP_CONFIG_CANCEL_HOLD_MS)`（1 秒，見 `button_input.c`）：按住 KEY0 一下會設定 `cancelled = true` 跳出迴圈，直接退回 BLE_RECEIVE、**不會呼叫 `storage_save_config()`**，不用真的把表單送出來才能離開。取消門檻（1 秒）刻意比進入 AP_CONFIG 用的門檻（3 秒）短，但不是瞬間單擊——連著的手機可能正在填表單，瞬間誤觸會讓熱點斷線、表單內容全部消失，需要一點保護。

**已修過的重要 bug**：表單「填了 SSID 卻永遠收到空字串」——真正原因是 HTTP headers 跟 body 常常被 TCP 拆成不同封包送達，舊邏輯看到 `\r\n\r\n` 就急著解析表單，那一刻 body 可能還沒送到。現在解析前會先比對 `Content-Length` 跟目前已收到的 body 長度，不夠就繼續等下一段 TCP 資料（`parse_content_length()`）。

### `src/mode_ble_receive.c` / `mode_ble_receive.h`
BLE 接收模式，24/7 常駐狀態。用 BTstack 當 GAP central：主動掃描（active scan，因為裝置名稱可能只在 scan response 封包裡）→ 掃到符合 `fora_protocol_matches_advertisement()` 的裝置就連線，同時記下是哪一種 `fora_device_kind_t`（額溫槍/血氧計/血壓計，見 PROJECT_PLAN.md 第 6 節）→ **血壓計會先走一次配對**（`sm_request_pairing()`，其餘兩種裝置不需要）→ 依裝置種類探索對應的服務/特徵值（**三種裝置現在都用同一套自訂 128-bit UUID pipe**，血壓計早期一度誤以為走標準 Blood Pressure Service，2026-08-05 反編譯官方程式確認其實跟另外兩種裝置共用同一套 Nordic LED/Button Service，見 PROJECT_PLAN.md 6.3/8.3 節）→ 依裝置種類分開快取 GATT handle（不同裝置的 attribute table 排列不同，handle 不能共用，這是修過的一個 bug）→ 訂閱 Notify 後主動送指令：額溫槍/血氧計送固定的觸發指令（`FORA_TRIGGER_COMMAND`）；血壓計要送**兩次**指令（`send_bp_get_record_part()`，`FORA_BP_CMD_GET_RECORD_PART_A`/`_B`）才能拼出完整一筆記錄，見 `GATT_EVENT_NOTIFICATION` 裡的兩段式合併邏輯（`s_bp_record_part_a`/`s_bp_waiting_for_part_b`）→ 收到數值後統一導向 `process_reading_payload()`，用 `fora_protocol_parse_reading()` 解析（一次可能解出 1~3 筆數值）、逐筆寫進 `storage_append_record()`、主動斷線。

開機時讀一次 `storage_load_config()` 存進 `s_display_config`，配合 `display_status_set_ble_receive()`／`display_status_poll()` 讓電子紙螢幕顯示個案編號、連線狀態文字、各生理值最後讀值、待傳筆數，見 `display_status.c` 說明跟 PROJECT_PLAN.md 12 節。**「Scanning」狀態文字改成帶時間戳的心跳**（`update_scanning_status()`），在 `BLE_STATE_SCANNING` 期間每 180 秒定期重新呼叫一次（`s_last_scanning_status_update_at`），讓時間戳在沒有裝置連線活動時依然前進，見 PROJECT_PLAN.md 12.6 節。

`DEVICE_RECONNECT_COOLDOWN_MS[]`：依裝置種類分開設定的冷卻時間，拿到讀值後這段時間內完全不理會該種裝置的廣播，避免裝置被無限重連導致無法休眠（跨 BLE_RECEIVE/UPLOAD 模式切換持續有效）。額溫槍/血壓計目前是 60 秒/4 分鐘，血氧計 5 秒——血壓計原本也是 5 秒，實測發現這台裝置在「傳送舊記錄」模式下設計是 3 分鐘無活動就自動關機，短冷卻會一直重置這個計時器讓裝置永遠關不了機，拉長到 4 分鐘（見 PROJECT_PLAN.md 8.4 節）。`s_got_any_reading_this_session`：修過的另一個 bug——原本「距離上次讀值 N 秒沒新資料就觸發上傳」的計時器是從「進入 BLE_RECEIVE 模式」就開始倒數，不是從「收到第一筆資料」開始，導致血壓計那種要 30-45 秒才會推播一次的裝置，量測還沒做完就被切去上傳模式、逼著斷線。現在改成收到第一筆資料之前完全不倒數；**且觸發上傳前會先確認 `storage_pending_count() > 0`**（判重邏輯上線後，收到讀值不代表待傳佇列真的有新東西，見 `storage.c` 說明），沒有東西要傳就不切去 UPLOAD 白跑一趟 WiFi。

**（2026-08-06 新增，尚未實機測試）** 回傳型別從 `bool` 改成 `mode_ble_receive_exit_t`（見 `mode_ble_receive.h`），主迴圈裡新增按鍵輪詢（`button_input.c`，見「板載按鍵」章節）：`button_input_key0_long_press(KEY0_ENTER_CONFIG_HOLD_MS)` 偵測到 KEY0 長按 3 秒就回傳 `MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG`，讓 `state_machine.c` 直接切去 AP_CONFIG，不用重開機找 BOOTSEL；`button_input_key1_pressed()` 偵測到 KEY1 按一下（**不要求待傳佇列非空**，2026-08-06 使用者確認「KEY1 = 完整一次 WiFi 動作」後拿掉這個條件）就呼叫 `wall_clock_request_resync()`（見 `wall_clock.c`）後直接回傳 `MODE_BLE_RECEIVE_EXIT_UPLOAD`，跳過 idle timeout 的等待，同時讓這次上傳前的 NTP 校時不受 6 小時節流限制。原本的 idle-timeout-with-pending-data 路徑不變（還是要求佇列非空），只是回傳值也改成同一個列舉值，`state_machine.c` 那邊的判斷邏輯相應更新。

**還沒校時成功時的自動重試（2026-08-06 加做，尚未實機測試）**：主迴圈新增獨立的 `s_last_ntp_retry_at` 計時器，不依賴 `s_got_any_reading_this_session`，只要 `!wall_clock_is_synced()` 且距離上次嘗試超過 `NTP_UNSYNCED_RETRY_MS`（5 分鐘）就回傳 `MODE_BLE_RECEIVE_EXIT_UPLOAD`，解決「裝置一直收不到任何 BLE 讀值就永遠沒機會嘗試 NTP」的問題，見 PROJECT_PLAN.md 第 2.4 節。

**KEY2（2026-08-06 加做，尚未實機測試）**：按一下呼叫 `storage_get_recent_upload_history()`／`storage_get_upload_history_count()` 取最近幾筆已上傳歷史，交給 `display_status_show_upload_history()` 畫出來，並把 `s_showing_history` 設成 true、記下 `s_history_view_until`（8 秒後的時間點）。主迴圈在 `s_showing_history` 為 true 期間**不呼叫** `display_status_poll()`（不然畫面會馬上被 BLE_RECEIVE 即時內容蓋掉），逾時後恢復正常輪詢，`poll()` 會因為 `display_status_show_upload_history()` 內部已經把 `s_ble_screen_is_current` 清成 false 而強制刷新一次換回即時畫面。BLE 掃描/連線本身走 BTstack 自己的事件回呼，不受這段暫停影響。

### `src/mode_upload.c` / `mode_upload.h`
上傳模式。先讀 flash 裡的裝置設定（`storage_load_config()`），沒設定過就直接放棄（點三連閃錯誤燈號）。有設定的話，用 `cyw43_arch_wifi_connect_async()` 開 WiFi station 模式連線，依序嘗試多種認證模式（`WIFI_AUTH_MODES_TO_TRY[]`，因為分享器種類很多，不寫死單一種）。連線成功的判斷**不是**單純信任 `cyw43_wifi_link_status()`（實測發現這個狀態有時候不會準時回報成功，即使 lwIP 的 DHCP 早就真的拿到 IP 了），而是直接檢查 `netif_is_up()` 且 netif 的 IP 不是 `0.0.0.0`——這是修過的一個關鍵 bug，詳見 PROJECT_PLAN.md 第 8 節第 1 點。連上後呼叫 `wall_clock_sync()` 嘗試 NTP 校時（見 `wall_clock.c`，**2026-08-06 改成不管這次有沒有資料要傳都會呼叫**，KEY1 手動觸發時就算佇列是空的也要能確認網路時間），再把 `storage_pending_records()` 取出的待傳紀錄丟給 `upload_api_post_batch()`（傳入 `config.upload_server_host`／`config.upload_api_key`）前逐筆換算時間戳：**有裝置自己認證過的量測時間戳（`device_measured_key != 0`，目前只有血壓計）就優先用 `fora_protocol_measured_key_to_epoch_ms()` 換算**（不依賴 NTP，實測過就算這次 NTP 校時失敗，血壓數值的時間戳依然正確），其餘裝置才用 `wall_clock_to_epoch_ms()`（校時失敗就維持 boot-relative）。依上傳結果用 `storage_mark_uploaded()` 標記成功或失敗（失敗的下次會重試），最後關掉 WiFi、回到 BLE 接收模式。

**（2026-08-06 加做，尚未實機測試）** `DEVICE_CLOCK_SANITY_WINDOW_MS`（7 天）：裝置自己的時間戳換算成 epoch ms 之後，如果 Pico 已經 NTP 校時過，會跟現在時間比對，差距超過這個範圍就判定裝置時鐘不可信（電池換過、從沒設定過等情況），退回用 Pico 收到 BLE 通知的時間換算；Pico 自己都還沒校時過的話沒有基準可以比對，照樣採用裝置時間戳。

---

## 藍牙協定解析

### `src/fora_protocol.c` / `fora_protocol.h`
三種 FORA 裝置的**實際**藍牙協定實作。完整協定細節（byte 格式、UUID、實測範例、血壓計協定的反編譯反推過程）見 **PROJECT_PLAN.md 第 6/8.3 節**，這裡只講程式碼結構：

- `fora_protocol_matches_advertisement()`：判斷掃到的廣播封包是不是 FORA 裝置（比對名稱含 "FORA"），並依名稱裡有沒有 "O2"／"D40" 進一步分辨是哪一種型號，透過 `fora_device_kind_t *out_kind` 回傳。
- `fora_protocol_build_command()`：組出三種裝置共用的 8-byte 指令格式 `{0x51, cmd, p1..p4, 0xA3, checksum}`，checksum 是前 7 bytes 總和的低位元組。血壓計用這個組「問記錄」的兩段式指令（`FORA_BP_CMD_GET_RECORD_PART_A`/`_B`）。
- `fora_protocol_parse_reading()`：依 `fora_device_kind_t` 決定用哪一種格式解析。額溫槍/血氧計是自訂的 `0x51` 開頭封包（借用 Nordic SDK 範例板的自訂 characteristic）；血壓計**不是**標準 Bluetooth SIG Blood Pressure Measurement 格式，是同一套自訂 pipe 的私有格式（呼叫端已經把兩次指令的回應接成 8 bytes 才傳進來，見 `mode_ble_receive.c` 的兩段式合併邏輯）。一次呼叫最多可能解出 3 筆數值（血壓計：收縮壓+舒張壓+脈搏），回傳實際筆數。每筆填入的 `vital_record_t` 都會設定 `source_kind`（記錄是哪種裝置回報的，給 `storage.c` 判重時區分共用型別如 `VITAL_TYPE_PULSE_RATE` 用）；血壓計還會額外填 `device_measured_key`（其餘裝置固定填 0）。**⚠️ 已知缺口**：FORA D40 是血壓血糖二合一裝置，「問目前這一筆記錄」的回應可能是血壓、也可能是血糖（回應 `byte[2]` 的 bit7 分辨，見 PROJECT_PLAN.md 第 6.3/6.4 節），目前這個函式完全沒有檢查這個 bit，無條件當成血壓解析——使用者量過一次血糖之後，下次血壓計連線讀到的「最新記錄」就會是那筆血糖資料被誤當成血壓數值上傳。血糖協定本身已經反推確認（PROJECT_PLAN.md 6.4 節），但解析函式裡還沒接上這個分流判斷，是目前優先權最高的待辦（PROJECT_PLAN.md 第 7.1 節第 1、2 點）。
- `fora_protocol_decode_measured_key()`：血壓記錄的 `byte[0..3]` 藏著裝置內部時鐘認證過的量測日期/時分（day/month/year/hour/minute，分鐘解析度），這裡解碼成一個可以直接比較是否相等的鍵值，給 `storage.c` 判重用（同一個鍵值保證是同一筆記錄）。
- `fora_protocol_measured_key_to_datetime()` / `fora_protocol_measured_key_to_epoch_ms()`：把上面的鍵值還原成年/月/日/時/分，或換算成 epoch ms（假設裝置內部時鐘存的是本地時間 UTC+8，`common.h` 的 `LOCAL_UTC_OFFSET_SEC`），分別給 `display_status.c` 畫面顯示、`mode_upload.c` 上傳用——這個時間戳代表「裝置實際量測的時間」，不是「Pico 收到 BLE 通知的時間」，兩者可能有延遲，而且完全不依賴 NTP 校時（實測驗證過 NTP 失敗時血壓的時間戳依然正確）。`days_from_civil()` 是 Howard Hinnant 的年/月/日換算天數演算法（CC0 授權），跟 `display_status.c` 的 `civil_month_day_from_days()` 是同一套演算法的另一半。

---

## 網路上傳實作

### `src/upload_api.c` / `upload_api.h`
把 `vital_record_t` 陣列組成 JSON、透過 HTTPS POST 送到伺服器。流程：`dns_gethostbyname()` 解析目標主機名 → 用 lwIP 的 `altcp_tls` 包一層 TLS 建立連線 → 用 `mbedtls_ssl_set_hostname()` 設定 SNI（Cloudflare 這類多租戶邊緣節點靠這個決定轉給哪個 tunnel）→ `altcp_write()` 送出組好的 HTTP request → 收回應判斷有沒有 `200`。這個檔案裡也提供了 `mbedtls_ms_time()` 的實作，因為 mbedtls 官方版本只支援 POSIX/Windows，在 Pico 這種 bare-metal 環境要自己接一個版本進去（用 pico SDK 的單調時鐘）。

**（2026-08-06 加做，尚未實機測試）** `upload_api_post_batch()` 簽名新增 `server_host`／`api_key` 兩個參數（來自 `device_config_t`，`mode_upload.c` 呼叫時傳入）：`server_host` 留空就退回檔案開頭的 `UPLOAD_SERVER_HOST_DEFAULT`（原本寫死的測試網址，現在只是預設值）；`api_key` 有值的話 `build_request()` 會加一個 `X-API-Key` 標頭（先過濾非可印出 ASCII 字元）。TLS 憑證驗證改成看 `upload_tls_ca_cert.h` 的 `UPLOAD_CA_CERT_PEM` 陣列有沒有內容：有內容就連同長度一起交給 `altcp_tls_create_config_client()`（lwIP 會自動改成要求驗證），沒有內容維持原本的不驗證模式，但現在每次都會在 log 印出明顯警告，不再是靜默的不安全狀態。

### `src/upload_tls_ca_cert.h`（2026-08-06 新增，尚未實機測試）
只有一個常數 `UPLOAD_CA_CERT_PEM`，目前是空字串——正式後端網址確定之後，把該伺服器的憑證/CA PEM 貼進這裡，`upload_api.c` 就會自動改成真正驗證憑證鏈，不用改其他程式碼。檔案裡有詳細的取得憑證方式說明（`openssl s_client`／`openssl x509` 指令）。

---

### `src/wall_clock.c` / `wall_clock.h`
Pico 開機時沒有網路，不知道真實時間，量測當下只能記錄開機以來的毫秒數（boot-relative）。這個檔案在 `mode_upload.c` 每次 WiFi 連上時嘗試跟公開的 NTP 伺服器（`pool.ntp.org`）校時一次，記住「校時那一刻的 boot ms 對應到哪個真實世界 epoch ms」，之後任何 boot-relative 時間點都能線性換算成真實時間。校時透過 lwIP 內建的 `pico_lwip_sntp` 函式庫，`lwipopts.h` 把 `SNTP_SET_SYSTEM_TIME_US` 巨集接到這個檔案裡的 `wall_clock_sntp_set_system_time_us()`。校時失敗（例如逾時）不影響上傳本身，只是那批資料的時間戳會維持 boot-relative 值。

**（2026-08-05 修 bug）** 原本 `s_synced` 成功過一次就永遠是 `true`，導致實際上只有開機後第一次呼叫 `wall_clock_sync()` 才會真的打 NTP，之後每次上傳都被誤判成「已經校過時」直接跳過，長時間運作下 RP2040 震盪器的漂移完全沒有機會被修正。改成每 `NTP_RESYNC_INTERVAL_MS`（目前 6 小時，2026-08-05 從 24 小時調整，頻率負擔仍很小，多留一點餘裕應付長時間運作的漂移累積）最多真的重新校時一次，並且伺服器改成清單（`pool.ntp.org`／`time.cloudflare.com`）輪流試，其中一個失敗換下一個；如果已經有過基準點、這次重新校時又失敗，會保留舊基準點而不是整個放棄，見 PROJECT_PLAN.md 第 7.2 節第 4 點。

**（2026-08-06 加做，尚未實機測試）** 新增 `wall_clock_request_resync()`／`s_force_resync` 旗標：`mode_ble_receive.c` 的 KEY1 手動觸發上傳時會呼叫這個函式，讓下一次 `wall_clock_sync()` 無視 `NTP_RESYNC_INTERVAL_MS` 節流、直接重新查詢，用過一次自動清除，不影響 idle-timeout 觸發的上傳路徑（那條路徑還是照 6 小時節流走）。

---

## 共用資料型別

### `src/common.h`
純資料定義，沒有任何函式實作。`device_config_t`（熱點設定模式收集的 WiFi 帳密＋個案資訊，**2026-08-06 新增** `upload_server_host`／`upload_api_key` 兩個欄位，見 `mode_ap_config.c`／`upload_api.c` 說明）、`vital_type_t`（體溫/血氧/脈搏/收縮壓/舒張壓/**血糖**，血糖協定已於 2026-08-06 反推確認並實作，見 PROJECT_PLAN.md 第 6.4 節）、`vital_record_t`（一筆量測紀錄：接收時間、數值、上傳時間、上傳狀態、`device_measured_key`、`source_kind`）。這三個型別貫穿 `storage.c`、`mode_ble_receive.c`、`mode_upload.c`、`upload_api.c`、`fora_protocol.c`，是串起整條資料流的共同語言。

`LOCAL_UTC_OFFSET_SEC`：這個專案目前只在台灣用，統一假設本地時間是 UTC+8，`display_status.c`（校時後換算真人看得懂的時鐘）跟 `fora_protocol.c`（血壓計自己時鐘的量測時間換算成 epoch ms）共用同一份常數，避免兩邊各自寫一份之後改一邊忘記改另一邊。

`vital_record_t.device_measured_key`：裝置自己回報的量測時間戳（分鐘解析度，`fora_protocol.c` 負責編碼/解碼），0 代表這種裝置的協定沒有這個資訊（目前只有血壓計有值）。`storage.c` 判重時如果雙方都有這個值就直接比對是否相等，不用再猜時間窗口。

`vital_record_t.source_kind`：記錄這筆讀值是哪種裝置回報的（`fora_protocol_parse_reading()` 填入，實際數值定義在 `fora_protocol.h` 的 `fora_device_kind_t`，這裡故意用不透明的 `uint8_t` 避免 common.h 反過來依賴 fora_protocol.h）。**加這個欄位的原因**：`VITAL_TYPE_PULSE_RATE` 同時被血氧計跟血壓計共用，如果判重只看 `vital_type_t` 不看是哪種裝置量的，兩種裝置的脈搏數值只要剛好相同/不同就會互相干擾對方的判重結果（實測抓到過一次：血壓計的脈搏覆蓋了血氧計的「最後讀值」，導致血氧計下一筆真正的重複讀值被誤判成新資料），見 PROJECT_PLAN.md 8.4 節第 19 點。

---

## 持久化與暫存

### `src/storage.c` / `storage.h`
三種資料都持久化在 littlefs（vendored v2.11.3，見下方 `littlefs/`／`lfs_pico_hal.c` 說明）掛載的一個 flash 分區裡，各自是分區裡的一個檔案：**裝置設定**（`device_config_t`）存在 `config.bin`，`mode_ap_config.c` 存、`mode_upload.c` 讀。**待傳生理資料**（`vital_record_t` 陣列，`MAX_PENDING_RECORDS 128`）存在 `pending.bin`（`persist_pending_records()`，`storage_append_record()`／`storage_mark_uploaded()` 呼叫時都會整份覆寫一次），開機時 `storage_init()` 會讀回，所以斷電或上傳失敗都不會遺失尚未成功上傳的資料。**已上傳歷史**（見下方說明）存在 `history.bin`。**一次性、不相容的格式改動**：這個分區原本是三個手刻的固定 sector，改成 littlefs 之後舊格式資料讀不出來，`lfs_mount()` 第一次掛載失敗會自動 `lfs_format()`，等於清空升級前留在 Pico 上的資料，見 PROJECT_PLAN.md 第 5.1 節。

**（2026-08-06 加做，尚未實機測試，主持人明確提出的需求）** `storage_mark_uploaded()` 原本上傳成功的紀錄會直接從待傳佇列移除、不再保留在任何地方。**主持人不希望上傳完後本機資料被直接清除**，新增 `s_upload_history[]`（`MAX_UPLOAD_HISTORY 200`，環狀緩衝，滿了覆蓋最舊一筆）跟對應的 `persist_upload_history()`／`storage_get_upload_history()`：上傳成功的紀錄除了從待傳佇列移除，也會複製一份進這個緩衝並持久化。**200 這個數字是概略估計**（假設單一個案一天約 20 筆讀值、涵蓋 1~2 週），不是精確計算或使用者指定的值，見 PROJECT_PLAN.md 第 7.3 節「待與相關人員確認事項」。

**（2026-08-06 加做，尚未實機測試）** 新增 `storage_get_upload_history_count()`（回傳目前總筆數）跟 `storage_get_recent_upload_history()`——跟 `storage_get_upload_history()` 從最舊開始取不同，這個是從最新往回取最近 N 筆（一樣最舊排前面、最新排最後），給 `mode_ble_receive.c` 的 KEY2 歷史畫面用，見「板載按鍵」章節。

`storage_pending_records()` 會撈出 `PENDING` 跟 `FAILED` 狀態的紀錄（不是只有 `PENDING`——之前這裡篩選條件寫錯，導致上傳失敗過一次的紀錄永遠不會再被重傳，是修過的一個 bug）。上傳失敗+斷電都不會遺失資料：失敗的紀錄留在佇列裡（標記 `FAILED`，不會被丟棄），只要佇列非空、BLE_RECEIVE 閒置就會被 `mode_ble_receive_run()` 觸發重新嘗試上傳，不需要額外的定時器；待傳佇列本身持久化在 flash，斷電重開機後 `storage_init()` 會讀回繼續重試。

**（2026-08-05 加的判重機制，見 `storage_append_record()`）** 裝置量測完常常會持續廣播一段時間，同一輪可能被連上好幾次，重複拿到「目前最新一筆」記錄。判重邏輯：先跟同類型（`vital_type_t`）**且同一種裝置回報**（`source_kind` 也要相同，見 `common.h` 的說明）的「最後讀值」比對——雙方都有裝置認證過的量測時間戳（`device_measured_key != 0`）就直接比對是否相等，沒有這個資訊的裝置（額溫槍/血氧計）才退回用「數值完全相同+時間間隔在 `DUPLICATE_SUPPRESS_WINDOW_MS`（10 分鐘）內」的經驗法則。判定為重複的話，畫面顯示用的 `s_last_reading` 照樣更新（時間戳往前推進，證明裝置剛剛還確認過這個數值仍是最新的），但不會塞進待傳佇列。

**（2026-08-05 新增，給電子紙畫面用）** `storage_get_last_reading()` / `storage_pending_count()`：待傳佇列裡的紀錄一旦上傳成功就會被 `storage_mark_uploaded()` 從陣列移除（見上面說明），但螢幕仍然需要顯示「最後量到多少」，所以另外用 `s_last_reading[VITAL_TYPE_COUNT]` 存一份「這次開機以來、不管有沒有上傳成功」的最後讀值，在 `storage_append_record()` 裡順便更新，只存在 RAM、不跨開機持久化（重開機後要等收到新讀值才會再有值）。同理，`storage_get_last_upload_time()` 記錄「這次開機以來最後一次成功上傳的時間」，在 `storage_mark_uploaded(success=true)` 時更新，一樣只在 RAM。

### `src/lfs_pico_hal.c` / `lfs_pico_hal.h`（2026-08-06 新增，尚未實機測試）
littlefs 跟 RP2040 flash 之間的 block-device 介面層，`storage.c` 掛載/格式化 littlefs 時用的 `lfs_pico_cfg`（`struct lfs_config`）就是這裡匯出的。分區固定劃在 flash 最後 `LFS_PICO_PARTITION_SIZE`（256KB，`lfs_pico_hal.h`）；一個 littlefs block 對應剛好一個實體 flash sector（4096 bytes）；讀取直接用 XIP 記憶體對映位址 `memcpy`，寫入/抹除透過 pico-sdk 的 `flash_safe_execute()` 包住 `flash_range_program`/`flash_range_erase`（RP2040 抹寫 flash 期間 XIP 無法使用，`flash_safe_execute()` 負責協調暫停其他核心/中斷）；`block_cycles = 500`（littlefs 官方建議範圍 100-1000 的中間值）。讀寫/lookahead 緩衝區都用靜態陣列，不使用 littlefs 內建的 `lfs_malloc()`，跟專案其他地方一律不用堆積配置的慣例一致。

### `littlefs/`
Vendored 自官方 [littlefs-project/littlefs](https://github.com/littlefs-project/littlefs) tag `v2.11.3`（BSD-3-Clause，`littlefs/LICENSE.md`），`lfs.c`/`lfs.h`（核心檔案系統邏輯）跟 `lfs_util.c`/`lfs_util.h`（工具函式，CRC/位元運算等）沒有做任何修改。這是一個嵌入式用的日誌式檔案系統，設計上就是給裸機/靜態配置的環境用，跟這個專案的需求（不能用堆積配置、要有 wear-leveling、要能在斷電時保持一致性）直接吻合。

---

## LED 狀態機

### `src/led_status.c` / `led_status.h`
Pico W 的板載 LED 接在 CYW43 晶片上而非一般 GPIO，只能透過 `cyw43_arch_gpio_put()` 控制，且必須在主迴圈裡呼叫、不能在中斷處理常式裡呼叫（PROJECT_PLAN.md 第 2.3 節：社群回報過在計時器 ISR 呼叫、同時網路忙碌會 hang 住）。這個檔案提供一個非阻塞的燈號狀態機：`led_status_set(pattern)` 切換燈號模式，`led_status_poll()` 要在各模式的主迴圈裡持續被呼叫才會真的閃爍（快閃/慢閃/心跳/三連閃錯誤燈號等，對照表見 PROJECT_PLAN.md 第 4 節）。

---

## 板載按鍵

### `src/button_input.c` / `button_input.h`（2026-08-06 新增，尚未實機測試）
讀取 Waveshare Pico-CapTouch-ePaper-2.9 板載的 KEY0（GP2）/KEY1（GP3）/KEY2（GP15）按鍵，見 PROJECT_PLAN.md 第 2.2/12.1/12.8 節。`main.c` 開機時呼叫一次 `button_input_init()`（設成內建上拉輸入，按下時接地為低電位）。`mode_ble_receive.c` 的主迴圈非阻塞輪詢：`button_input_key0_long_press(hold_ms)` 要連續按住達 `hold_ms` 才回傳一次 true（放開才重新計時，同一次按住不會重複觸發），`button_input_key1_pressed()`／`button_input_key2_pressed()` 都是邊緣觸發（偵測到「這次按下、上次沒按下」才回傳 true）。**因為現場可能有沒接這片電子紙的機器**，GP2/GP3/GP15 沒有實體按鍵接上去的話讀到的一律是上拉的高電位（沒按下），不會誤觸發，不需要編譯選項區分兩種硬體。這片板子的電容觸控（I2C1，見 PROJECT_PLAN.md 12.1 節）目前還沒有用到。

---

## 電子紙顯示器

### `src/display_status.c` / `display_status.h`
Waveshare Pico-ePaper-2.9 電子紙顯示器封裝，跟 `led_status.c` 平行存在但目的不同：LED 是隨時看得到、需要背燈號規則才懂的低階指示，這片螢幕的目標是讓個管師/家屬不用懂技術也看得懂裝置狀態。**Phase 1+2 都已於 2026-08-05 完整燒錄實機驗證**（驅動接線/字型/方向，以及四個模式的真實資料畫面），四個模式各自對應：

- `display_status_show_ap_config()`：熱點 SSID/密碼 + 目前已存的個案編號（ASCII 過濾過，見下方）+ WiFi QR code（`draw_qr_code()`，見下方 `qrcode/` 說明），`mode_ap_config.c` 開熱點後呼叫一次，內容整個設定階段不會變，不需要輪詢。
- `display_status_show_upload()` / `display_status_show_error()`：`mode_upload.c` 在連線中/成功/失敗等幾個關鍵時間點呼叫，一樣是直接畫、直接刷新。
- `display_status_show_upload_history()`（2026-08-06 加做，尚未實機測試）：`mode_ble_receive.c` 按 KEY2 時呼叫，畫最多 7 筆最近已上傳歷史（`vital_label()`/`vital_unit()` 順便補上原本缺的 `VITAL_TYPE_SYSTOLIC`/`DIASTOLIC` case），一樣是直接畫、直接刷新，呼叫端負責計時多久之後要換回即時畫面，見「板載按鍵」章節。
- `display_status_set_ble_receive()` + `display_status_poll()`：`mode_ble_receive.c` 的主迴圈是持續數十秒到數分鐘的緊迴圈，內容（狀態文字、各生理值的最後讀值、待傳筆數）會持續變動，用類似 `led_status_set()`/`led_status_poll()` 的呼叫慣例——`set_ble_receive()` 只是更新「想顯示的內容」（便宜），實際要不要刷新畫面由 `poll()`（跟 `led_status_poll()` 一樣每輪主迴圈呼叫）內部比對「這次的內容」跟「上次真的畫到螢幕上的內容」決定，只有真的不一樣才觸發一次全刷（全刷要 3 秒、會阻塞主迴圈，不能每輪都刷，見 PROJECT_PLAN.md 12.5 節）。**這個比對刻意不包含任何「距今 N 分鐘」這種會隨時間漂移的文字**，只比對原始數值/時間戳/筆數/狀態文字/`device_measured_key`，所以畫面上顯示的時間戳是「上次刷新當下」算出來的，不是即時的——這是為了不讓時間流逝本身觸發刷新風暴的刻意簡化。

**時間戳顯示邏輯（`draw_reading_row()` 內部的 `format_reading_clock()`）**：有裝置自己認證過的量測時間戳（`device_measured_key != 0`，目前只有血壓計）就優先顯示裝置實際量測的時間（`fora_protocol_measured_key_to_datetime()` 直接解碼成 `MM/DD HH:MM`，不經過 wall_clock 換算），其餘裝置（額溫槍/血氧計）才用 `display_status_format_clock()` 顯示「Pico 收到 BLE 通知的時間」（校時過顯示絕對時間，沒校時顯示 `unsynced,+Nm`）。**血糖列**（`VITAL_TYPE_GLUCOSE`）協定已接上並實作（見 PROJECT_PLAN.md 第 6.4 節），量到血糖時會正常顯示，尚未實機驗證過；沒量過的話跟其他生理值一樣顯示 `-- (never)`。

**「Scanning」狀態文字的心跳**：故意不用靜態文字，會帶時間戳（`mode_ble_receive.c` 的 `update_scanning_status()`），且在沒有裝置連線活動時每 180 秒定期重新呼叫一次，讓使用者能從畫面判斷「裝置還活著、只是沒掃到裝置」跟「裝置已經當機、畫面凍結」的差別。

**（硬體規格書要求，見 PROJECT_PLAN.md 12.5.1 節）** `end_frame_and_refresh(void)` 是四種畫面共用的唯一刷新入口：電子紙不能長時間維持通電/高電壓是硬性規定，每次刷新都是「喚醒 → 畫 → `EPD_2IN9_V2_Sleep()`」，兩次刷新之間面板永遠在睡眠狀態。資料手冊建議的「刷新間隔至少 180 秒」**這條建議值本身刻意沒有嚴格遵守，以使用方便性為原則**（真的有新資料/狀態要顯示時不delay），但保留了「距離上次刷新超過 24 小時就算內容沒變也強制刷一次」；上面提到的「Scanning」心跳是唯一一個「定期、沒有實際新事件也會觸發」的刷新來源，這個改成比照資料手冊建議的 180 秒，跟「180 秒不嚴格遵守」的決定不衝突（那個決定針對的是有新事件時不要延遲，心跳沒有這個顧慮）。

**（2026-08-06 曾經加過 Phase 3 局部刷新又拿掉了）** 一度加了 `prefer_partial` 參數讓 BLE_RECEIVE 畫面優先用 `EPD_2IN9_V2_Display_Partial()`。重新檢視後發現沒有任何實測觀察到的具體場景真的需要它（原本設想「全刷卡住 2-3 秒可能讓 BLE 主迴圈錯過裝置短暫廣播窗口」只是理論推測，從沒真的觀察到發生過），局部刷新換來的速度/不閃黑好處要用「沒有實機驗證過的呼叫順序」跟「疊代殘影需要額外清理邏輯」這些真實風險去換，不值得，決定拿掉、只保留全刷（`EPD_2IN9_V2_Display_Base()`），見 PROJECT_PLAN.md 第 12.5 節。

`display_status.c` 內部有一個 `sanitize_ascii()`：使用者在 AP_CONFIG 表單填的 `patient_id` 沒有限制輸入內容，理論上可能填中文，但目前只有 ASCII 字型（見下方 `epd/` 說明），直接把非 ASCII byte 丟給 `Paint_DrawChar()` 可能亂碼甚至讀到無效 flash 位址——所有使用者自由輸入的欄位在畫到螢幕前都會先過濾成只剩可印出 ASCII 字元。**呼叫端自己組的狀態文字/錯誤訊息不會被這個函式過濾，必須自己確保是純英文**（`display_status.h` 檔頭有這個限制的說明）。

畫最後讀值需要「查某個 `vital_type_t` 最後一筆數值」，這個查詢介面 (`storage_get_last_reading()`) 是這次順便加進 `storage.c` 的，見下方說明。規劃細節、中文顯示方案（已決定用個案編號取代姓名）、全刷/局部刷新的取捨，見 **PROJECT_PLAN.md 第 12 節**。

**（2026-08-05 新增）** `display_status_show_ap_config()` 現在會在畫面右側多畫一個 WiFi QR code，手機相機掃到會直接跳出「加入 WiFi」的系統提示（連的是 Pico 自己的設定用熱點 `AP_SSID`/`AP_PASSWORD`，不是個案要接的目標 WiFi）。用 `qrcode/qrcodegen.c` 編碼、`display_status.c` 裡的 `draw_qr_code()` 把每個 module 放大畫成實際像素。見 PROJECT_PLAN.md 12.7 節。

### `qrcode/`（從 Nayuki `QR-Code-generator` 複製進來的第三方 QR code 編碼器）
不是我們自己寫的，是從 Nayuki 的 `QR-Code-generator` GitHub repo 的 `c/` 目錄複製過來（跟 `epd/`、`dhcpserver.c` 一樣的做法），只有 `qrcodegen.c`/`.h` 兩個檔案，MIT 授權，純 C89、沒有外部依賴，沒有改動任何一行。只給 `display_status.c` 產生 AP_CONFIG 畫面的 WiFi QR code 用。

### `epd/`（從 Waveshare `Pico_ePaper_Code` 複製進來的第三方驅動）
不是我們自己寫的，是從 Waveshare 官方 GitHub repo 的 `c/lib/` 複製進專案根目錄（跟 `dhcpserver.c`/`dnsserver.c` 一樣的做法）：
- `DEV_Config.c/.h`：SPI1（`spi1`，硬體 SPI）+ GPIO 底層存取。**本地改過三處**：拿掉 `DEV_Module_Init()` 內重複的 `stdio_init_all()`（`main.c` 已經呼叫過）；把 `gpio_set_function()` 的參數從原版依賴巧合的 `GPIO_OUT` 改成明確的 `GPIO_FUNC_SPI`；**（2026-08-06，尚未實機測試）** `DEV_GPIO_Init()` 幫 `EPD_BUSY_PIN` 加 `gpio_pull_down()`，見下方 `EPD_2in9_V2.c` 的說明。
- `EPD_2in9_V2.c/.h`：SSD1680 控制器指令層，提供全刷（`EPD_2IN9_V2_Display`/`Display_Base`，~3 秒）、局部刷新（`EPD_2IN9_V2_Display_Partial`，~0.6 秒但會有殘影，目前沒用到，見 PROJECT_PLAN.md 12.5 節）、睡眠（`EPD_2IN9_V2_Sleep`）。`EPD_2IN9_V2_ReadBusy()` 是阻塞式忙等 BUSY 腳位，全刷會讓主迴圈卡住 2-3 秒。**本地改過一處（2026-08-06，尚未實機測試）**：原版 `ReadBusy()` 沒有逾時、沒接電子紙板時 BUSY 腳位電位未定義，可能永久卡住整個韌體（開機後第一次畫面刷新就會發生），加了 5 秒逾時上限，搭配上面 `DEV_Config.c` 的內建下拉讓沒接面板時能立刻返回，詳見 PROJECT_PLAN.md 第 12.9 節「無螢幕版本相容性」。
- `GUI_Paint.c/.h`：畫面 framebuffer（1 bpp）+ 畫點/線/框/文字 API。
- `Fonts/`：只帶了官方 demo 裡的 ASCII 字型（`font8/12/16/20/24`），**沒有帶**簡體中文字型（`font12CN`/`font24CN`）——這兩個檔案是 GB2312 編碼，而且只內建 demo 用到的幾個字，對這個專案沒用。個案姓名這類自由輸入的繁體中文要怎麼顯示，還沒決定（見 PROJECT_PLAN.md 12.3 節的三個方案）。

---

## 從 pico-examples 複製進來的第三方工具

### `dhcpserver.c` / `dhcpserver.h`、`dnsserver.c` / `dnsserver.h`
不是我們自己寫的，是從官方 `pico-examples/pico_w/wifi/access_point_wifi_provisioning/` 複製進專案根目錄的共用工具程式（pico-sdk 本身不內建，這兩個檔案不會隨 SDK 一起裝好，是專案自己額外複製、加進 `CMakeLists.txt` 的編譯來源）。`mode_ap_config.c` 開熱點時靠這兩個檔案讓連上來的手機自動拿到 IP（DHCP）並且不管查什麼網域都導回 Pico 自己（DNS，做出類似公用 WiFi 「登入頁」的效果）。

---

## 建置與函式庫設定檔（決定編譯出來的行為，但本身不是程式邏輯）

### `CMakeLists.txt`
定義要編譯哪些原始檔（見上面 `add_executable()` 的清單，就是這份文件列出的所有 `src/*.c` 加上 `dhcpserver.c`/`dnsserver.c`/`epd/` 底下的電子紙驅動/`qrcode/qrcodegen.c`/`littlefs/lfs.c`/`littlefs/lfs_util.c`）、要連結哪些 pico-sdk 函式庫（`pico_btstack_ble`＝藍牙、`pico_cyw43_arch_lwip_threadsafe_background`＝WiFi、`pico_mbedtls`＋`pico_lwip_mbedtls`＝上傳用的 TLS、`pico_lwip_sntp`＝ NTP 校時、`hardware_flash`＋`pico_flash`＝`lfs_pico_hal.c` 用來實作 littlefs 的 flash 讀寫/抹除、`pico_unique_id`＝ AP 熱點密碼衍生、`hardware_spi`＝電子紙走的 SPI1）、`target_include_directories` 額外加了 `littlefs/` 目錄、以及一些編譯期開關（USB 序列埠開、UART 序列埠關；`PICO_MBEDTLS_CONFIG_FILE` 指到我們自己的 mbedtls 設定檔）。

### `pico_sdk_import.cmake`
pico-sdk 官方樣板檔案，負責找到 `PICO_SDK_PATH` 指的 SDK 位置並載入它，內容跟我們的產品邏輯無關，是每個 pico-sdk 專案都會有的固定樣板。

### `lwipopts.h`
lwIP（TCP/IP 協定棧）的編譯期設定，決定要打開哪些協定（DHCP、DNS、TCP、UDP）、緩衝區大小（`TCP_MSS`、`TCP_WND` 等）。`LWIP_ALTCP`／`LWIP_ALTCP_TLS`／`LWIP_ALTCP_TLS_MBEDTLS` 是 `upload_api.c` 能用 TLS 連線的前提。`SNTP_SERVER_DNS` + `SNTP_SET_SYSTEM_TIME_US` 巨集是 `wall_clock.c` 的 NTP 校時能動的前提（後者接到 `wall_clock.c` 裡的回呼函式，需要在這個檔案裡 forward-declare）。除錯 WiFi/DHCP 連線問題時可以打開 `DHCP_DEBUG`／`NETIF_DEBUG`（目前註解掉，而且**要注意這兩個巨集必須放在 `#ifndef NDEBUG` 判斷之外**才會真的生效，因為這個 project 是 Release build 一定會定義 `NDEBUG`——這是之前踩過的坑，加了 debug 巨集卻放錯位置，一直看不到任何 log 輸出）。

### `btstack_config.h`
BTstack（藍牙協定棧）的編譯期設定，決定要開哪些藍牙功能（只開 `ENABLE_LE_CENTRAL`，因為我們只需要連線週邊裝置，不需要 BLE peripheral 或傳統藍牙）跟各種 buffer/連線數上限。改編自 pico-sdk 官方測試專案，大部分巨集保留跟參考範例一致以避免拿掉後在 pico-sdk 內部檔案冒出新的缺巨集編譯錯誤。

### `mbedtls_config_override.h`
本次對話新加的檔案，客製化 mbedtls（TLS 函式庫）的編譯設定，取代 pico-sdk 內建的預設版本。存在的必要性：mbedtls 官方預設值大量假設「跑在 Linux/Windows 上」（亂數來源、計時函式、憑證檔案讀取都預設呼叫作業系統 API），在 Pico 這種 bare-metal 環境會直接編譯錯誤，這個檔案逐一關掉那些用不到的 OS 相依功能、接上 pico-sdk 提供的硬體替代實作（硬體亂數），並縮小 TLS 緩衝區大小配合 RP2040 只有 264KB RAM 的限制。每一段設定為什麼需要，檔案裡都有對應註解。
