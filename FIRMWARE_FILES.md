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
BLE 接收模式，24/7 常駐狀態。用 BTstack 當 GAP central：主動掃描（active scan，因為裝置名稱可能只在 scan response 封包裡）→ 掃到符合 `fora_protocol_matches_advertisement()` 的裝置就連線，同時記下是哪一種 `fora_device_kind_t`（額溫槍/血氧計/血壓計，三種裝置服務/特徵值 UUID 跟取值方式不同，見 PROJECT_PLAN.md 第 6 節）→ 依裝置種類探索對應的服務/特徵值（額溫槍/血氧計用自訂 128-bit UUID，血壓計用標準 16-bit UUID）→ 依裝置種類分開快取 GATT handle（兩種自訂 pipe 裝置的 attribute table 排列不同，handle 不能共用，這是修過的一個 bug）→ 取值方式依裝置種類不同：額溫槍/血氧計訂閱 Notify 後送觸發指令才會回傳；血壓計**直接 Read** 特徵值（原本照標準規格訂閱 Indicate，但這台裝置量測完才開藍芽，連線建立時量測早就結束了，Indicate 只推播「訂閱之後」的事件所以永遠等不到，改成直接 Read 才驗證成功，見 PROJECT_PLAN.md 第 6.3 節）→ 收到數值後（透過 `GATT_EVENT_NOTIFICATION` 或 `GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT`，兩種都會導向同一個共用函式 `process_reading_payload()`）用 `fora_protocol_parse_reading()` 解析（一次可能解出 1~3 筆數值）、逐筆寫進 `storage_append_record()`、主動斷線。

`s_have_reading[]`（依裝置種類分開追蹤）：某種裝置這一輪拿到讀值後，即使還在廣播也不再重新連線，避免裝置被無限重連導致無法休眠。`s_got_any_reading_this_session`：修過的另一個 bug——原本「距離上次讀值 N 秒沒新資料就觸發上傳」的計時器是從「進入 BLE_RECEIVE 模式」就開始倒數，不是從「收到第一筆資料」開始，導致血壓計那種要 30-45 秒才會推播一次的裝置，量測還沒做完就被切去上傳模式、逼著斷線。現在改成收到第一筆資料之前完全不倒數。

### `src/mode_upload.c` / `mode_upload.h`
上傳模式。先讀 flash 裡的裝置設定（`storage_load_config()`），沒設定過就直接放棄（點三連閃錯誤燈號）。有設定的話，用 `cyw43_arch_wifi_connect_async()` 開 WiFi station 模式連線，依序嘗試多種認證模式（`WIFI_AUTH_MODES_TO_TRY[]`，因為分享器種類很多，不寫死單一種）。連線成功的判斷**不是**單純信任 `cyw43_wifi_link_status()`（實測發現這個狀態有時候不會準時回報成功，即使 lwIP 的 DHCP 早就真的拿到 IP 了），而是直接檢查 `netif_is_up()` 且 netif 的 IP 不是 `0.0.0.0`——這是修過的一個關鍵 bug，詳見 PROJECT_PLAN.md 第 8 節第 1 點。連上後呼叫 `wall_clock_sync()` 嘗試 NTP 校時（見 `wall_clock.c`），再把 `storage_pending_records()` 取出的待傳紀錄（時間戳換算成真實世界時間，校時失敗就維持 boot-relative）丟給 `upload_api_post_batch()`，依回傳結果用 `storage_mark_uploaded()` 標記成功或失敗（失敗的下次會重試），最後關掉 WiFi、回到 BLE 接收模式。

---

## 藍牙協定解析

### `src/fora_protocol.c` / `fora_protocol.h`
三種 FORA 裝置的**實際**藍牙協定實作。完整協定細節（byte 格式、UUID、實測範例）見 **PROJECT_PLAN.md 第 6 節**，這裡只講程式碼結構：

- `fora_protocol_matches_advertisement()`：判斷掃到的廣播封包是不是 FORA 裝置（比對名稱含 "FORA"），並依名稱裡有沒有 "O2"／"D40" 進一步分辨是哪一種型號，透過 `fora_device_kind_t *out_kind` 回傳。
- `fora_protocol_parse_reading()`：依 `fora_device_kind_t` 決定用哪一種格式解析，額溫槍/血氧計是自訂的 `0x51` 開頭封包（借用 Nordic SDK 範例板的自訂 characteristic，額溫槍是標準 Bluetooth SIG Health Thermometer profile 宣告了但根本不是真正資料管道的經典案例），血壓計是標準 Bluetooth SIG Blood Pressure Measurement 格式（IEEE-11073 SFLOAT，`decode_sfloat()` 這個檔案裡自己實作的小函式）。一次呼叫最多可能解出 3 筆數值（血壓計：收縮壓+舒張壓+脈搏），回傳實際筆數。

---

## 網路上傳實作

### `src/upload_api.c` / `upload_api.h`
把 `vital_record_t` 陣列組成 JSON、透過 HTTPS POST 送到伺服器。流程：`dns_gethostbyname()` 解析目標主機名 → 用 lwIP 的 `altcp_tls` 包一層 TLS 建立連線 → 用 `mbedtls_ssl_set_hostname()` 設定 SNI（Cloudflare 這類多租戶邊緣節點靠這個決定轉給哪個 tunnel）→ `altcp_write()` 送出組好的 HTTP request → 收回應判斷有沒有 `200`。伺服器位址（`UPLOAD_SERVER_HOST`）目前寫死在檔案開頭常數，因為 `device_config_t` 還沒有存這個欄位的地方，是刻意的測試階段簡化（見檔案裡的 TODO 註解）。這個檔案裡也提供了 `mbedtls_ms_time()` 的實作，因為 mbedtls 官方版本只支援 POSIX/Windows，在 Pico 這種 bare-metal 環境要自己接一個版本進去（用 pico SDK 的單調時鐘）。

---

### `src/wall_clock.c` / `wall_clock.h`
Pico 開機時沒有網路，不知道真實時間，量測當下只能記錄開機以來的毫秒數（boot-relative）。這個檔案在 `mode_upload.c` 每次 WiFi 連上時嘗試跟公開的 NTP 伺服器（`pool.ntp.org`）校時一次，記住「校時那一刻的 boot ms 對應到哪個真實世界 epoch ms」，之後任何 boot-relative 時間點都能線性換算成真實時間。校時透過 lwIP 內建的 `pico_lwip_sntp` 函式庫，`lwipopts.h` 把 `SNTP_SET_SYSTEM_TIME_US` 巨集接到這個檔案裡的 `wall_clock_sntp_set_system_time_us()`。校時失敗（例如逾時）不影響上傳本身，只是那批資料的時間戳會維持 boot-relative 值。

---

## 共用資料型別

### `src/common.h`
純資料定義，沒有任何函式實作。`device_config_t`（熱點設定模式收集的 WiFi 帳密＋個案資訊）、`vital_type_t`（體溫/血氧/脈搏列舉）、`vital_record_t`（一筆量測紀錄：接收時間、數值、上傳時間、上傳狀態）。這三個型別貫穿 `storage.c`、`mode_ble_receive.c`、`mode_upload.c`、`upload_api.c`，是串起整條資料流的共同語言。

---

## 持久化與暫存

### `src/storage.c` / `storage.h`
兩種資料都會寫進 flash，用兩個不同的保留 sector 區塊分開存，互不影響：**裝置設定**（`device_config_t`）存在最後一個 sector，`mode_ap_config.c` 存、`mode_upload.c` 讀。**待傳生理資料**（`vital_record_t` 陣列，`MAX_PENDING_RECORDS 128`）存在再往前保留的幾個 sector（`persist_pending_records()`，`storage_append_record()`／`storage_mark_uploaded()` 呼叫時都會整份覆寫一次），開機時 `storage_init()` 會讀回，所以斷電或上傳失敗都不會遺失尚未成功上傳的資料。兩者都用 `flash_safe_execute()` 安全地抹寫，讀取直接用 XIP 位址存取。**注意：這個做法沒有 wear-leveling**，正式量產前如果讀值頻率提高，需評估升級成 littlefs（見 PROJECT_PLAN.md 第 8 節第 7 點）。

`storage_pending_records()` 會撈出 `PENDING` 跟 `FAILED` 狀態的紀錄（不是只有 `PENDING`——之前這裡篩選條件寫錯，導致上傳失敗過一次的紀錄永遠不會再被重傳，是修過的一個 bug）。

---

## LED 狀態機

### `src/led_status.c` / `led_status.h`
Pico W 的板載 LED 接在 CYW43 晶片上而非一般 GPIO，只能透過 `cyw43_arch_gpio_put()` 控制，且必須在主迴圈裡呼叫、不能在中斷處理常式裡呼叫（PROJECT_PLAN.md 第 2.3 節：社群回報過在計時器 ISR 呼叫、同時網路忙碌會 hang 住）。這個檔案提供一個非阻塞的燈號狀態機：`led_status_set(pattern)` 切換燈號模式，`led_status_poll()` 要在各模式的主迴圈裡持續被呼叫才會真的閃爍（快閃/慢閃/心跳/三連閃錯誤燈號等，對照表見 PROJECT_PLAN.md 第 4 節）。

---

## 從 pico-examples 複製進來的第三方工具

### `dhcpserver.c` / `dhcpserver.h`、`dnsserver.c` / `dnsserver.h`
不是我們自己寫的，是從官方 `pico-examples/pico_w/wifi/access_point_wifi_provisioning/` 複製進專案根目錄的共用工具程式（pico-sdk 本身不內建，這兩個檔案不會隨 SDK 一起裝好，是專案自己額外複製、加進 `CMakeLists.txt` 的編譯來源）。`mode_ap_config.c` 開熱點時靠這兩個檔案讓連上來的手機自動拿到 IP（DHCP）並且不管查什麼網域都導回 Pico 自己（DNS，做出類似公用 WiFi 「登入頁」的效果）。

---

## 建置與函式庫設定檔（決定編譯出來的行為，但本身不是程式邏輯）

### `CMakeLists.txt`
定義要編譯哪些原始檔（見上面 `add_executable()` 的清單，就是這份文件列出的所有 `src/*.c` 加上 `dhcpserver.c`/`dnsserver.c`）、要連結哪些 pico-sdk 函式庫（`pico_btstack_ble`＝藍牙、`pico_cyw43_arch_lwip_threadsafe_background`＝WiFi、`pico_mbedtls`＋`pico_lwip_mbedtls`＝上傳用的 TLS、`pico_lwip_sntp`＝ NTP 校時、`hardware_flash`＋`pico_flash`＝設定值/待傳紀錄持久化）、以及一些編譯期開關（USB 序列埠開、UART 序列埠關；`PICO_MBEDTLS_CONFIG_FILE` 指到我們自己的 mbedtls 設定檔）。

### `pico_sdk_import.cmake`
pico-sdk 官方樣板檔案，負責找到 `PICO_SDK_PATH` 指的 SDK 位置並載入它，內容跟我們的產品邏輯無關，是每個 pico-sdk 專案都會有的固定樣板。

### `lwipopts.h`
lwIP（TCP/IP 協定棧）的編譯期設定，決定要打開哪些協定（DHCP、DNS、TCP、UDP）、緩衝區大小（`TCP_MSS`、`TCP_WND` 等）。`LWIP_ALTCP`／`LWIP_ALTCP_TLS`／`LWIP_ALTCP_TLS_MBEDTLS` 是 `upload_api.c` 能用 TLS 連線的前提。`SNTP_SERVER_DNS` + `SNTP_SET_SYSTEM_TIME_US` 巨集是 `wall_clock.c` 的 NTP 校時能動的前提（後者接到 `wall_clock.c` 裡的回呼函式，需要在這個檔案裡 forward-declare）。除錯 WiFi/DHCP 連線問題時可以打開 `DHCP_DEBUG`／`NETIF_DEBUG`（目前註解掉，而且**要注意這兩個巨集必須放在 `#ifndef NDEBUG` 判斷之外**才會真的生效，因為這個 project 是 Release build 一定會定義 `NDEBUG`——這是之前踩過的坑，加了 debug 巨集卻放錯位置，一直看不到任何 log 輸出）。

### `btstack_config.h`
BTstack（藍牙協定棧）的編譯期設定，決定要開哪些藍牙功能（只開 `ENABLE_LE_CENTRAL`，因為我們只需要連線週邊裝置，不需要 BLE peripheral 或傳統藍牙）跟各種 buffer/連線數上限。改編自 pico-sdk 官方測試專案，大部分巨集保留跟參考範例一致以避免拿掉後在 pico-sdk 內部檔案冒出新的缺巨集編譯錯誤。

### `mbedtls_config_override.h`
本次對話新加的檔案，客製化 mbedtls（TLS 函式庫）的編譯設定，取代 pico-sdk 內建的預設版本。存在的必要性：mbedtls 官方預設值大量假設「跑在 Linux/Windows 上」（亂數來源、計時函式、憑證檔案讀取都預設呼叫作業系統 API），在 Pico 這種 bare-metal 環境會直接編譯錯誤，這個檔案逐一關掉那些用不到的 OS 相依功能、接上 pico-sdk 提供的硬體替代實作（硬體亂數），並縮小 TLS 緩衝區大小配合 RP2040 只有 264KB RAM 的限制。每一段設定為什麼需要，檔案裡都有對應註解。
