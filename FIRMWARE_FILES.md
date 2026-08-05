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
整個裝置的主控迴圈：`state_machine_run()` 先呼叫 `mode_boot_select_check()` 決定第一個狀態，之後在 `STATE_AP_CONFIG`／`STATE_BLE_RECEIVE`／`STATE_UPLOAD` 三個狀態間切換（見上面的流程圖）。**同一時間只允許一種無線模式運作**（WiFi 或藍牙擇一），這個規則靠 `radio_switch_to_wifi()` / `radio_switch_to_bluetooth()` 兩個函式統一把關，避免以後改個別模式時不小心兩個無線一起開（PROJECT_PLAN.md 第 2.1 節有解釋為什麼要這樣設計：RAM 有限、WiFi+BLE 同開的穩定度在社群案例裡不夠多）。

---

## 開機模式選擇

### `src/mode_boot_select.c` / `mode_boot_select.h`
Pico W 沒有使用者按鍵，只有 BOOTSEL。這個檔案用一個取自官方範例的技巧（暫時把 QSPI_SS 腳位切成輸入、讀電位、再切回去）在**開機後、韌體正常執行中**偵測 BOOTSEL 有沒有被按著——注意這跟「開機時按著 BOOTSEL 進 USB 燒錄模式」是兩回事，那是晶片 boot ROM 的行為，跟這裡讀 GPIO 的程式碼完全無關，這裡的偵測是韌體已經開始跑之後才做的。`mode_boot_select_check(window_ms)` 在指定的毫秒數視窗內（目前 4 秒，LED 快閃提示），按住就回傳 `true`（進熱點設定模式），沒按就回傳 `false`（進 BLE 接收模式，也是預設的 24/7 常駐狀態）。

---

## 三個運作模式

### `src/mode_ap_config.c` / `mode_ap_config.h`
「熱點設定模式」。開一個獨立的 WiFi 熱點（`AP_SSID "PicoGateway-Setup"` / `AP_PASSWORD "gateway123"`，目前寫死在程式碼裡，PROJECT_PLAN.md 有註記正式版要改成每台裝置唯一密碼），搭配 `dhcpserver.c`／`dnsserver.c` 讓手機連上後自動配到 IP，並且不管查什麼網域都導回 Pico 自己觸發作業系統的 captive portal 自動彈出登入頁（模仿公用 WiFi 的行為，使用者不用自己開瀏覽器打網址）。進入這個模式時會先 `scan_nearby_wifi()` 掃描附近 WiFi，設定頁的 SSID 欄位是下拉選單（掃到的網路，含訊號強度）+ 一個手動輸入欄位（給掃不到的隱藏網路用）。用 lwIP 的 raw TCP API 手刻一個極簡單的單連線 HTTP server（不是官方的 lwIP httpd），GET 回傳表單、POST 收表單資料後用 `storage_save_config()` 寫進 flash，然後結束（回到 BLE 接收模式）。

表單會先讀目前已存的設定（`storage_load_config()`）帶入頁面：SSID 下拉選單自動選中目前的網路、個案姓名/編號/個管師資訊會帶入現有值，**密碼欄位留空 = 不變更目前密碼**（不會被清空覆蓋），方便使用者只改其中一項而不用重填全部欄位。

**這個模式跟「Pico 實際運作時要連的 WiFi」是兩個獨立網路**：這個熱點只是拿來讓你在設定介面上「填入」目標 WiFi 的帳密，Pico 存好設定後就會關掉這個熱點、改用你填的帳密去連目標 WiFi。

**已修過的重要 bug**：表單「填了 SSID 卻永遠收到空字串」——真正原因是 HTTP headers 跟 body 常常被 TCP 拆成不同封包送達，舊邏輯看到 `\r\n\r\n` 就急著解析表單，那一刻 body 可能還沒送到。現在解析前會先比對 `Content-Length` 跟目前已收到的 body 長度，不夠就繼續等下一段 TCP 資料（`parse_content_length()`）。

### `src/mode_ble_receive.c` / `mode_ble_receive.h`
BLE 接收模式，24/7 常駐狀態。用 BTstack 當 GAP central：主動掃描（active scan，因為裝置名稱可能只在 scan response 封包裡）→ 掃到符合 `fora_protocol_matches_advertisement()` 的裝置就連線，同時記下是哪一種 `fora_device_kind_t`（額溫槍/血氧計/血壓計，見 PROJECT_PLAN.md 第 6 節）→ **血壓計會先走一次配對**（`sm_request_pairing()`，其餘兩種裝置不需要）→ 依裝置種類探索對應的服務/特徵值（**三種裝置現在都用同一套自訂 128-bit UUID pipe**，血壓計早期一度誤以為走標準 Blood Pressure Service，2026-08-05 反編譯官方程式確認其實跟另外兩種裝置共用同一套 Nordic LED/Button Service，見 PROJECT_PLAN.md 6.3/8.3 節）→ 依裝置種類分開快取 GATT handle（不同裝置的 attribute table 排列不同，handle 不能共用，這是修過的一個 bug）→ 訂閱 Notify 後主動送指令：額溫槍/血氧計送固定的觸發指令（`FORA_TRIGGER_COMMAND`）；血壓計要送**兩次**指令（`send_bp_get_record_part()`，`FORA_BP_CMD_GET_RECORD_PART_A`/`_B`）才能拼出完整一筆記錄，見 `GATT_EVENT_NOTIFICATION` 裡的兩段式合併邏輯（`s_bp_record_part_a`/`s_bp_waiting_for_part_b`）→ 收到數值後統一導向 `process_reading_payload()`，用 `fora_protocol_parse_reading()` 解析（一次可能解出 1~3 筆數值）、逐筆寫進 `storage_append_record()`、主動斷線。

開機時讀一次 `storage_load_config()` 存進 `s_display_config`，配合 `display_status_set_ble_receive()`／`display_status_poll()` 讓電子紙螢幕顯示個案編號、連線狀態文字、各生理值最後讀值、待傳筆數，見 `display_status.c` 說明跟 PROJECT_PLAN.md 12 節。**「Scanning」狀態文字改成帶時間戳的心跳**（`update_scanning_status()`），在 `BLE_STATE_SCANNING` 期間每 180 秒定期重新呼叫一次（`s_last_scanning_status_update_at`），讓時間戳在沒有裝置連線活動時依然前進，見 PROJECT_PLAN.md 12.6 節。

`DEVICE_RECONNECT_COOLDOWN_MS[]`：依裝置種類分開設定的冷卻時間，拿到讀值後這段時間內完全不理會該種裝置的廣播，避免裝置被無限重連導致無法休眠（跨 BLE_RECEIVE/UPLOAD 模式切換持續有效）。額溫槍/血壓計目前是 60 秒/4 分鐘，血氧計 5 秒——血壓計原本也是 5 秒，實測發現這台裝置在「傳送舊記錄」模式下設計是 3 分鐘無活動就自動關機，短冷卻會一直重置這個計時器讓裝置永遠關不了機，拉長到 4 分鐘（見 PROJECT_PLAN.md 8.4 節）。`s_got_any_reading_this_session`：修過的另一個 bug——原本「距離上次讀值 N 秒沒新資料就觸發上傳」的計時器是從「進入 BLE_RECEIVE 模式」就開始倒數，不是從「收到第一筆資料」開始，導致血壓計那種要 30-45 秒才會推播一次的裝置，量測還沒做完就被切去上傳模式、逼著斷線。現在改成收到第一筆資料之前完全不倒數；**且觸發上傳前會先確認 `storage_pending_count() > 0`**（判重邏輯上線後，收到讀值不代表待傳佇列真的有新東西，見 `storage.c` 說明），沒有東西要傳就不切去 UPLOAD 白跑一趟 WiFi。

### `src/mode_upload.c` / `mode_upload.h`
上傳模式。先讀 flash 裡的裝置設定（`storage_load_config()`），沒設定過就直接放棄（點三連閃錯誤燈號）。有設定的話，用 `cyw43_arch_wifi_connect_async()` 開 WiFi station 模式連線，依序嘗試多種認證模式（`WIFI_AUTH_MODES_TO_TRY[]`，因為分享器種類很多，不寫死單一種）。連線成功的判斷**不是**單純信任 `cyw43_wifi_link_status()`（實測發現這個狀態有時候不會準時回報成功，即使 lwIP 的 DHCP 早就真的拿到 IP 了），而是直接檢查 `netif_is_up()` 且 netif 的 IP 不是 `0.0.0.0`——這是修過的一個關鍵 bug，詳見 PROJECT_PLAN.md 第 8 節第 1 點。連上後呼叫 `wall_clock_sync()` 嘗試 NTP 校時（見 `wall_clock.c`），再把 `storage_pending_records()` 取出的待傳紀錄丟給 `upload_api_post_batch()` 前逐筆換算時間戳：**有裝置自己認證過的量測時間戳（`device_measured_key != 0`，目前只有血壓計）就優先用 `fora_protocol_measured_key_to_epoch_ms()` 換算**（不依賴 NTP，實測過就算這次 NTP 校時失敗，血壓數值的時間戳依然正確），其餘裝置才用 `wall_clock_to_epoch_ms()`（校時失敗就維持 boot-relative）。依上傳結果用 `storage_mark_uploaded()` 標記成功或失敗（失敗的下次會重試），最後關掉 WiFi、回到 BLE 接收模式。

---

## 藍牙協定解析

### `src/fora_protocol.c` / `fora_protocol.h`
三種 FORA 裝置的**實際**藍牙協定實作。完整協定細節（byte 格式、UUID、實測範例、血壓計協定的反編譯反推過程）見 **PROJECT_PLAN.md 第 6/8.3 節**，這裡只講程式碼結構：

- `fora_protocol_matches_advertisement()`：判斷掃到的廣播封包是不是 FORA 裝置（比對名稱含 "FORA"），並依名稱裡有沒有 "O2"／"D40" 進一步分辨是哪一種型號，透過 `fora_device_kind_t *out_kind` 回傳。
- `fora_protocol_build_command()`：組出三種裝置共用的 8-byte 指令格式 `{0x51, cmd, p1..p4, 0xA3, checksum}`，checksum 是前 7 bytes 總和的低位元組。血壓計用這個組「問記錄」的兩段式指令（`FORA_BP_CMD_GET_RECORD_PART_A`/`_B`）。
- `fora_protocol_parse_reading()`：依 `fora_device_kind_t` 決定用哪一種格式解析。額溫槍/血氧計是自訂的 `0x51` 開頭封包（借用 Nordic SDK 範例板的自訂 characteristic）；血壓計**不是**標準 Bluetooth SIG Blood Pressure Measurement 格式，是同一套自訂 pipe 的私有格式（呼叫端已經把兩次指令的回應接成 8 bytes 才傳進來，見 `mode_ble_receive.c` 的兩段式合併邏輯）。一次呼叫最多可能解出 3 筆數值（血壓計：收縮壓+舒張壓+脈搏），回傳實際筆數。每筆填入的 `vital_record_t` 都會設定 `source_kind`（記錄是哪種裝置回報的，給 `storage.c` 判重時區分共用型別如 `VITAL_TYPE_PULSE_RATE` 用）；血壓計還會額外填 `device_measured_key`（其餘裝置固定填 0）。
- `fora_protocol_decode_measured_key()`：血壓記錄的 `byte[0..3]` 藏著裝置內部時鐘認證過的量測日期/時分（day/month/year/hour/minute，分鐘解析度），這裡解碼成一個可以直接比較是否相等的鍵值，給 `storage.c` 判重用（同一個鍵值保證是同一筆記錄）。
- `fora_protocol_measured_key_to_datetime()` / `fora_protocol_measured_key_to_epoch_ms()`：把上面的鍵值還原成年/月/日/時/分，或換算成 epoch ms（假設裝置內部時鐘存的是本地時間 UTC+8，`common.h` 的 `LOCAL_UTC_OFFSET_SEC`），分別給 `display_status.c` 畫面顯示、`mode_upload.c` 上傳用——這個時間戳代表「裝置實際量測的時間」，不是「Pico 收到 BLE 通知的時間」，兩者可能有延遲，而且完全不依賴 NTP 校時（實測驗證過 NTP 失敗時血壓的時間戳依然正確）。`days_from_civil()` 是 Howard Hinnant 的年/月/日換算天數演算法（CC0 授權），跟 `display_status.c` 的 `civil_month_day_from_days()` 是同一套演算法的另一半。

---

## 網路上傳實作

### `src/upload_api.c` / `upload_api.h`
把 `vital_record_t` 陣列組成 JSON、透過 HTTPS POST 送到伺服器。流程：`dns_gethostbyname()` 解析目標主機名 → 用 lwIP 的 `altcp_tls` 包一層 TLS 建立連線 → 用 `mbedtls_ssl_set_hostname()` 設定 SNI（Cloudflare 這類多租戶邊緣節點靠這個決定轉給哪個 tunnel）→ `altcp_write()` 送出組好的 HTTP request → 收回應判斷有沒有 `200`。伺服器位址（`UPLOAD_SERVER_HOST`）目前寫死在檔案開頭常數，因為 `device_config_t` 還沒有存這個欄位的地方，是刻意的測試階段簡化（見檔案裡的 TODO 註解）。這個檔案裡也提供了 `mbedtls_ms_time()` 的實作，因為 mbedtls 官方版本只支援 POSIX/Windows，在 Pico 這種 bare-metal 環境要自己接一個版本進去（用 pico SDK 的單調時鐘）。

---

### `src/wall_clock.c` / `wall_clock.h`
Pico 開機時沒有網路，不知道真實時間，量測當下只能記錄開機以來的毫秒數（boot-relative）。這個檔案在 `mode_upload.c` 每次 WiFi 連上時嘗試跟公開的 NTP 伺服器（`pool.ntp.org`）校時一次，記住「校時那一刻的 boot ms 對應到哪個真實世界 epoch ms」，之後任何 boot-relative 時間點都能線性換算成真實時間。校時透過 lwIP 內建的 `pico_lwip_sntp` 函式庫，`lwipopts.h` 把 `SNTP_SET_SYSTEM_TIME_US` 巨集接到這個檔案裡的 `wall_clock_sntp_set_system_time_us()`。校時失敗（例如逾時）不影響上傳本身，只是那批資料的時間戳會維持 boot-relative 值。

**（2026-08-05 修 bug）** 原本 `s_synced` 成功過一次就永遠是 `true`，導致實際上只有開機後第一次呼叫 `wall_clock_sync()` 才會真的打 NTP，之後每次上傳都被誤判成「已經校過時」直接跳過，長時間運作下 RP2040 震盪器的漂移完全沒有機會被修正。改成每天最多真的重新校時一次（`NTP_RESYNC_INTERVAL_MS`），並且伺服器改成清單（`pool.ntp.org`／`time.cloudflare.com`）輪流試，其中一個失敗換下一個；如果已經有過基準點、這次重新校時又失敗，會保留舊基準點而不是整個放棄，見 PROJECT_PLAN.md 第 7 節第 3 點。

---

## 共用資料型別

### `src/common.h`
純資料定義，沒有任何函式實作。`device_config_t`（熱點設定模式收集的 WiFi 帳密＋個案資訊）、`vital_type_t`（體溫/血氧/脈搏/收縮壓/舒張壓/**血糖**，血糖欄位是保留給以後用的，協定還沒接）、`vital_record_t`（一筆量測紀錄：接收時間、數值、上傳時間、上傳狀態、`device_measured_key`、`source_kind`）。這三個型別貫穿 `storage.c`、`mode_ble_receive.c`、`mode_upload.c`、`upload_api.c`、`fora_protocol.c`，是串起整條資料流的共同語言。

`LOCAL_UTC_OFFSET_SEC`：這個專案目前只在台灣用，統一假設本地時間是 UTC+8，`display_status.c`（校時後換算真人看得懂的時鐘）跟 `fora_protocol.c`（血壓計自己時鐘的量測時間換算成 epoch ms）共用同一份常數，避免兩邊各自寫一份之後改一邊忘記改另一邊。

`vital_record_t.device_measured_key`：裝置自己回報的量測時間戳（分鐘解析度，`fora_protocol.c` 負責編碼/解碼），0 代表這種裝置的協定沒有這個資訊（目前只有血壓計有值）。`storage.c` 判重時如果雙方都有這個值就直接比對是否相等，不用再猜時間窗口。

`vital_record_t.source_kind`：記錄這筆讀值是哪種裝置回報的（`fora_protocol_parse_reading()` 填入，實際數值定義在 `fora_protocol.h` 的 `fora_device_kind_t`，這裡故意用不透明的 `uint8_t` 避免 common.h 反過來依賴 fora_protocol.h）。**加這個欄位的原因**：`VITAL_TYPE_PULSE_RATE` 同時被血氧計跟血壓計共用，如果判重只看 `vital_type_t` 不看是哪種裝置量的，兩種裝置的脈搏數值只要剛好相同/不同就會互相干擾對方的判重結果（實測抓到過一次：血壓計的脈搏覆蓋了血氧計的「最後讀值」，導致血氧計下一筆真正的重複讀值被誤判成新資料），見 PROJECT_PLAN.md 8.4 節第 19 點。

---

## 持久化與暫存

### `src/storage.c` / `storage.h`
兩種資料都會寫進 flash，用兩個不同的保留 sector 區塊分開存，互不影響：**裝置設定**（`device_config_t`）存在最後一個 sector，`mode_ap_config.c` 存、`mode_upload.c` 讀。**待傳生理資料**（`vital_record_t` 陣列，`MAX_PENDING_RECORDS 128`）存在再往前保留的幾個 sector（`persist_pending_records()`，`storage_append_record()`／`storage_mark_uploaded()` 呼叫時都會整份覆寫一次），開機時 `storage_init()` 會讀回，所以斷電或上傳失敗都不會遺失尚未成功上傳的資料。兩者都用 `flash_safe_execute()` 安全地抹寫，讀取直接用 XIP 位址存取。**注意：這個做法沒有 wear-leveling**，正式量產前如果讀值頻率提高，需評估升級成 littlefs（見 PROJECT_PLAN.md 第 8 節第 7 點）。

`storage_pending_records()` 會撈出 `PENDING` 跟 `FAILED` 狀態的紀錄（不是只有 `PENDING`——之前這裡篩選條件寫錯，導致上傳失敗過一次的紀錄永遠不會再被重傳，是修過的一個 bug）。上傳失敗+斷電都不會遺失資料：失敗的紀錄留在佇列裡（標記 `FAILED`，不會被丟棄），只要佇列非空、BLE_RECEIVE 閒置就會被 `mode_ble_receive_run()` 觸發重新嘗試上傳，不需要額外的定時器；待傳佇列本身持久化在 flash，斷電重開機後 `storage_init()` 會讀回繼續重試。

**（2026-08-05 加的判重機制，見 `storage_append_record()`）** 裝置量測完常常會持續廣播一段時間，同一輪可能被連上好幾次，重複拿到「目前最新一筆」記錄。判重邏輯：先跟同類型（`vital_type_t`）**且同一種裝置回報**（`source_kind` 也要相同，見 `common.h` 的說明）的「最後讀值」比對——雙方都有裝置認證過的量測時間戳（`device_measured_key != 0`）就直接比對是否相等，沒有這個資訊的裝置（額溫槍/血氧計）才退回用「數值完全相同+時間間隔在 `DUPLICATE_SUPPRESS_WINDOW_MS`（10 分鐘）內」的經驗法則。判定為重複的話，畫面顯示用的 `s_last_reading` 照樣更新（時間戳往前推進，證明裝置剛剛還確認過這個數值仍是最新的），但不會塞進待傳佇列。

**（2026-08-05 新增，給電子紙畫面用）** `storage_get_last_reading()` / `storage_pending_count()`：待傳佇列裡的紀錄一旦上傳成功就會被 `storage_mark_uploaded()` 從陣列移除（見上面說明），但螢幕仍然需要顯示「最後量到多少」，所以另外用 `s_last_reading[VITAL_TYPE_COUNT]` 存一份「這次開機以來、不管有沒有上傳成功」的最後讀值，在 `storage_append_record()` 裡順便更新，只存在 RAM、不跨開機持久化（重開機後要等收到新讀值才會再有值）。同理，`storage_get_last_upload_time()` 記錄「這次開機以來最後一次成功上傳的時間」，在 `storage_mark_uploaded(success=true)` 時更新，一樣只在 RAM。

---

## LED 狀態機

### `src/led_status.c` / `led_status.h`
Pico W 的板載 LED 接在 CYW43 晶片上而非一般 GPIO，只能透過 `cyw43_arch_gpio_put()` 控制，且必須在主迴圈裡呼叫、不能在中斷處理常式裡呼叫（PROJECT_PLAN.md 第 2.3 節：社群回報過在計時器 ISR 呼叫、同時網路忙碌會 hang 住）。這個檔案提供一個非阻塞的燈號狀態機：`led_status_set(pattern)` 切換燈號模式，`led_status_poll()` 要在各模式的主迴圈裡持續被呼叫才會真的閃爍（快閃/慢閃/心跳/三連閃錯誤燈號等，對照表見 PROJECT_PLAN.md 第 4 節）。

---

## 電子紙顯示器

### `src/display_status.c` / `display_status.h`
Waveshare Pico-ePaper-2.9 電子紙顯示器封裝，跟 `led_status.c` 平行存在但目的不同：LED 是隨時看得到、需要背燈號規則才懂的低階指示，這片螢幕的目標是讓個管師/家屬不用懂技術也看得懂裝置狀態。**Phase 1+2 都已於 2026-08-05 完整燒錄實機驗證**（驅動接線/字型/方向，以及四個模式的真實資料畫面），四個模式各自對應：

- `display_status_show_ap_config()`：熱點 SSID/密碼 + 目前已存的個案編號（ASCII 過濾過，見下方）+ WiFi QR code（`draw_qr_code()`，見下方 `qrcode/` 說明），`mode_ap_config.c` 開熱點後呼叫一次，內容整個設定階段不會變，不需要輪詢。
- `display_status_show_upload()` / `display_status_show_error()`：`mode_upload.c` 在連線中/成功/失敗等幾個關鍵時間點呼叫，一樣是直接畫、直接刷新。
- `display_status_set_ble_receive()` + `display_status_poll()`：`mode_ble_receive.c` 的主迴圈是持續數十秒到數分鐘的緊迴圈，內容（狀態文字、各生理值的最後讀值、待傳筆數）會持續變動，用類似 `led_status_set()`/`led_status_poll()` 的呼叫慣例——`set_ble_receive()` 只是更新「想顯示的內容」（便宜），實際要不要刷新畫面由 `poll()`（跟 `led_status_poll()` 一樣每輪主迴圈呼叫）內部比對「這次的內容」跟「上次真的畫到螢幕上的內容」決定，只有真的不一樣才觸發一次全刷（全刷要 3 秒、會阻塞主迴圈，不能每輪都刷，見 PROJECT_PLAN.md 12.5 節）。**這個比對刻意不包含任何「距今 N 分鐘」這種會隨時間漂移的文字**，只比對原始數值/時間戳/筆數/狀態文字/`device_measured_key`，所以畫面上顯示的時間戳是「上次刷新當下」算出來的，不是即時的——這是為了不讓時間流逝本身觸發刷新風暴的刻意簡化。

**時間戳顯示邏輯（`draw_reading_row()` 內部的 `format_reading_clock()`）**：有裝置自己認證過的量測時間戳（`device_measured_key != 0`，目前只有血壓計）就優先顯示裝置實際量測的時間（`fora_protocol_measured_key_to_datetime()` 直接解碼成 `MM/DD HH:MM`，不經過 wall_clock 換算），其餘裝置（額溫槍/血氧計）才用 `display_status_format_clock()` 顯示「Pico 收到 BLE 通知的時間」（校時過顯示絕對時間，沒校時顯示 `unsynced,+Nm`）。**血糖列**（`VITAL_TYPE_GLUCOSE`）目前永遠顯示 `-- (never)`，協定還沒接（見 PROJECT_PLAN.md 第 7 節第 2 點），只是先把畫面欄位留著。

**「Scanning」狀態文字的心跳**：故意不用靜態文字，會帶時間戳（`mode_ble_receive.c` 的 `update_scanning_status()`），且在沒有裝置連線活動時每 180 秒定期重新呼叫一次，讓使用者能從畫面判斷「裝置還活著、只是沒掃到裝置」跟「裝置已經當機、畫面凍結」的差別。

**（硬體規格書要求，見 PROJECT_PLAN.md 12.5.1 節）** `end_frame_and_refresh()` 是四種畫面共用的唯一刷新入口：電子紙不能長時間維持通電/高電壓是硬性規定，每次刷新都是「喚醒 → 畫 → `EPD_2IN9_V2_Sleep()`」，兩次刷新之間面板永遠在睡眠狀態。資料手冊建議的「刷新間隔至少 180 秒」**這條建議值本身刻意沒有嚴格遵守，以使用方便性為原則**（真的有新資料/狀態要顯示時不delay），但保留了「距離上次刷新超過 24 小時就算內容沒變也強制刷一次」；上面提到的「Scanning」心跳是唯一一個「定期、沒有實際新事件也會觸發」的刷新來源，這個改成比照資料手冊建議的 180 秒，跟「180 秒不嚴格遵守」的決定不衝突（那個決定針對的是有新事件時不要延遲，心跳沒有這個顧慮）。

`display_status.c` 內部有一個 `sanitize_ascii()`：使用者在 AP_CONFIG 表單填的 `patient_id` 沒有限制輸入內容，理論上可能填中文，但目前只有 ASCII 字型（見下方 `epd/` 說明），直接把非 ASCII byte 丟給 `Paint_DrawChar()` 可能亂碼甚至讀到無效 flash 位址——所有使用者自由輸入的欄位在畫到螢幕前都會先過濾成只剩可印出 ASCII 字元。**呼叫端自己組的狀態文字/錯誤訊息不會被這個函式過濾，必須自己確保是純英文**（`display_status.h` 檔頭有這個限制的說明）。

畫最後讀值需要「查某個 `vital_type_t` 最後一筆數值」，這個查詢介面 (`storage_get_last_reading()`) 是這次順便加進 `storage.c` 的，見下方說明。規劃細節、中文顯示方案（已決定用個案編號取代姓名）、全刷/局部刷新的取捨，見 **PROJECT_PLAN.md 第 12 節**。

**（2026-08-05 新增）** `display_status_show_ap_config()` 現在會在畫面右側多畫一個 WiFi QR code，手機相機掃到會直接跳出「加入 WiFi」的系統提示（連的是 Pico 自己的設定用熱點 `AP_SSID`/`AP_PASSWORD`，不是個案要接的目標 WiFi）。用 `qrcode/qrcodegen.c` 編碼、`display_status.c` 裡的 `draw_qr_code()` 把每個 module 放大畫成實際像素。見 PROJECT_PLAN.md 12.7 節。

### `qrcode/`（從 Nayuki `QR-Code-generator` 複製進來的第三方 QR code 編碼器）
不是我們自己寫的，是從 Nayuki 的 `QR-Code-generator` GitHub repo 的 `c/` 目錄複製過來（跟 `epd/`、`dhcpserver.c` 一樣的做法），只有 `qrcodegen.c`/`.h` 兩個檔案，MIT 授權，純 C89、沒有外部依賴，沒有改動任何一行。只給 `display_status.c` 產生 AP_CONFIG 畫面的 WiFi QR code 用。

### `epd/`（從 Waveshare `Pico_ePaper_Code` 複製進來的第三方驅動）
不是我們自己寫的，是從 Waveshare 官方 GitHub repo 的 `c/lib/` 複製進專案根目錄（跟 `dhcpserver.c`/`dnsserver.c` 一樣的做法）：
- `DEV_Config.c/.h`：SPI1（`spi1`，硬體 SPI）+ GPIO 底層存取。**本地改過兩處**：拿掉 `DEV_Module_Init()` 內重複的 `stdio_init_all()`（`main.c` 已經呼叫過），以及把 `gpio_set_function()` 的參數從原版依賴巧合的 `GPIO_OUT` 改成明確的 `GPIO_FUNC_SPI`。
- `EPD_2in9_V2.c/.h`：SSD1680 控制器指令層，提供全刷（`EPD_2IN9_V2_Display`/`Display_Base`，~3 秒）、局部刷新（`EPD_2IN9_V2_Display_Partial`，~0.6 秒但會有殘影）、睡眠（`EPD_2IN9_V2_Sleep`）。`EPD_2IN9_V2_ReadBusy()` 是阻塞式忙等 BUSY 腳位，全刷會讓主迴圈卡住 2-3 秒，Phase 2 排即時資料畫面時要注意別在 BLE 接收迴圈裡頻繁觸發。
- `GUI_Paint.c/.h`：畫面 framebuffer（1 bpp）+ 畫點/線/框/文字 API。
- `Fonts/`：只帶了官方 demo 裡的 ASCII 字型（`font8/12/16/20/24`），**沒有帶**簡體中文字型（`font12CN`/`font24CN`）——這兩個檔案是 GB2312 編碼，而且只內建 demo 用到的幾個字，對這個專案沒用。個案姓名這類自由輸入的繁體中文要怎麼顯示，還沒決定（見 PROJECT_PLAN.md 12.3 節的三個方案）。

---

## 從 pico-examples 複製進來的第三方工具

### `dhcpserver.c` / `dhcpserver.h`、`dnsserver.c` / `dnsserver.h`
不是我們自己寫的，是從官方 `pico-examples/pico_w/wifi/access_point_wifi_provisioning/` 複製進專案根目錄的共用工具程式（pico-sdk 本身不內建，這兩個檔案不會隨 SDK 一起裝好，是專案自己額外複製、加進 `CMakeLists.txt` 的編譯來源）。`mode_ap_config.c` 開熱點時靠這兩個檔案讓連上來的手機自動拿到 IP（DHCP）並且不管查什麼網域都導回 Pico 自己（DNS，做出類似公用 WiFi 「登入頁」的效果）。

---

## 建置與函式庫設定檔（決定編譯出來的行為，但本身不是程式邏輯）

### `CMakeLists.txt`
定義要編譯哪些原始檔（見上面 `add_executable()` 的清單，就是這份文件列出的所有 `src/*.c` 加上 `dhcpserver.c`/`dnsserver.c`/`epd/` 底下的電子紙驅動/`qrcode/qrcodegen.c`）、要連結哪些 pico-sdk 函式庫（`pico_btstack_ble`＝藍牙、`pico_cyw43_arch_lwip_threadsafe_background`＝WiFi、`pico_mbedtls`＋`pico_lwip_mbedtls`＝上傳用的 TLS、`pico_lwip_sntp`＝ NTP 校時、`hardware_flash`＋`pico_flash`＝設定值/待傳紀錄持久化、`hardware_spi`＝電子紙走的 SPI1）、以及一些編譯期開關（USB 序列埠開、UART 序列埠關；`PICO_MBEDTLS_CONFIG_FILE` 指到我們自己的 mbedtls 設定檔）。

### `pico_sdk_import.cmake`
pico-sdk 官方樣板檔案，負責找到 `PICO_SDK_PATH` 指的 SDK 位置並載入它，內容跟我們的產品邏輯無關，是每個 pico-sdk 專案都會有的固定樣板。

### `lwipopts.h`
lwIP（TCP/IP 協定棧）的編譯期設定，決定要打開哪些協定（DHCP、DNS、TCP、UDP）、緩衝區大小（`TCP_MSS`、`TCP_WND` 等）。`LWIP_ALTCP`／`LWIP_ALTCP_TLS`／`LWIP_ALTCP_TLS_MBEDTLS` 是 `upload_api.c` 能用 TLS 連線的前提。`SNTP_SERVER_DNS` + `SNTP_SET_SYSTEM_TIME_US` 巨集是 `wall_clock.c` 的 NTP 校時能動的前提（後者接到 `wall_clock.c` 裡的回呼函式，需要在這個檔案裡 forward-declare）。除錯 WiFi/DHCP 連線問題時可以打開 `DHCP_DEBUG`／`NETIF_DEBUG`（目前註解掉，而且**要注意這兩個巨集必須放在 `#ifndef NDEBUG` 判斷之外**才會真的生效，因為這個 project 是 Release build 一定會定義 `NDEBUG`——這是之前踩過的坑，加了 debug 巨集卻放錯位置，一直看不到任何 log 輸出）。

### `btstack_config.h`
BTstack（藍牙協定棧）的編譯期設定，決定要開哪些藍牙功能（只開 `ENABLE_LE_CENTRAL`，因為我們只需要連線週邊裝置，不需要 BLE peripheral 或傳統藍牙）跟各種 buffer/連線數上限。改編自 pico-sdk 官方測試專案，大部分巨集保留跟參考範例一致以避免拿掉後在 pico-sdk 內部檔案冒出新的缺巨集編譯錯誤。

### `mbedtls_config_override.h`
本次對話新加的檔案，客製化 mbedtls（TLS 函式庫）的編譯設定，取代 pico-sdk 內建的預設版本。存在的必要性：mbedtls 官方預設值大量假設「跑在 Linux/Windows 上」（亂數來源、計時函式、憑證檔案讀取都預設呼叫作業系統 API），在 Pico 這種 bare-metal 環境會直接編譯錯誤，這個檔案逐一關掉那些用不到的 OS 相依功能、接上 pico-sdk 提供的硬體替代實作（硬體亂數），並縮小 TLS 緩衝區大小配合 RP2040 只有 264KB RAM 的限制。每一段設定為什麼需要，檔案裡都有對應註解。
