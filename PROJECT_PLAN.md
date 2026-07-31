# Raspberry Pi Pico W 生理訊號中繼裝置 — 專案計畫

## 目前進度（2026-07-31）

- ✅ 開發環境（pico-sdk v2.3.0、ARM 工具鏈、CMake、Ninja、MinGW）已裝好，可在本機編譯燒錄。
- ✅ 專案骨架（狀態機、BOOTSEL 開機視窗、LED 燈號、flash/RAM 儲存）已建立，實機驗證運作正常。
- ✅ **BLE 接收模式已完整打通，實機驗證能正確收到真實體溫數值**（FORA IR42 實測 37.00°C）。實際協定跟一開始從官方標準文件假設的完全不同，細節記錄在第 7.1 節，之後接其他裝置時務必先看過。
- ✅ `dhcpserver.c/.h`、`dnsserver.c/.h` 已複製進專案根目錄，`mode_ap_config.c` 的相依已補齊（但整體尚未實機驗證，見下）。
- ✅ 本機測試用上傳伺服器 `test_server/app.py`（純 Python 標準函式庫，免安裝）已寫好並驗證能收 POST、顯示網頁，見 `test_server/README.md` 的暫定 API 格式。
- ✅ 專案已建立 git repo 並備份到 GitHub private repo：`https://github.com/chanwunkong/pico-vitals-gateway`。
- ⬜ **上傳模式（`upload_api.c`）尚未實作**：還沒接上 `test_server`，需要用 lwIP `altcp` API 手刻 HTTP POST（見第 7 節第 2 點）。這是下次接續開發建議優先做的項目。
- ⬜ 熱點設定模式（`mode_ap_config.c`）程式碼已寫好但尚未實機驗證。
- ⬜ 待傳資料目前只放在 RAM，尚未落地到 flash（見第 7 節第 4 點）。
- ⬜ 已知問題：裝置量測後會持續廣播一段時間，目前每次掃到都會重新連線觸發，同一次量測可能被重複記錄成好幾筆一樣的資料，尚未加防重複機制（見第 7 節第 8 點）。

## 0. 在新電腦上接續開發

這份專案的 git 內容（原始碼、`PROJECT_PLAN.md`、`README.md`）已經備份到 GitHub，但**開發環境（pico-sdk、工具鏈）不在 git 裡**，換一台電腦要重新裝。步驟：

1. `git clone https://github.com/chanwunkong/pico-vitals-gateway.git`（private repo，要先用 `gh auth login` 或個人帳號登入過 git 才 clone 得到）。
2. 裝 VSCode 官方 **"Raspberry Pi Pico"** 擴充套件，跑一次 **Import Pico Project** 指向這個資料夾，讓它自動下載 SDK/工具鏈（最簡單，交給官方工具處理）。
   - 或手動照這次的做法：`winget install Kitware.CMake`、`winget install Ninja-build.Ninja`、`winget install Arm.GnuArmEmbeddedToolchain`、`winget install BrechtSanders.WinLibs.POSIX.UCRT`（host 端編譯 pioasm/picotool 需要），再 `git clone --branch 2.3.0 --depth 1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git` 到本機任一路徑，並把該路徑設成使用者環境變數 `PICO_SDK_PATH`。
3. 編譯：`cmake -S . -B build -G Ninja` 然後 `cmake --build build`。
4. 燒錄：裝置接電腦、進 BOOTSEL 模式（若已在跑舊韌體，可用 `picotool reboot -u -f` 觸發），把 `build/pico_gateway.uf2` 複製到出現的 `RPI-RP2` 磁碟機。
5. 除錯：VSCode 裝 **Serial Monitor** 擴充套件（`ms-vscode.vscode-serial-monitor`）看即時 log。

下次建議先做的事：把 `upload_api.c` 接上 `test_server/app.py`（先在同一台機器上跑 server + 燒錄韌體測試，確認上傳流程能動之後，再確認 Pico 之後實際部署環境的 WiFi 是否能連到目標 API）。

## 1. 產品概述

一台以 Raspberry Pi Pico W 為核心的中繼裝置，安裝於個案床邊，負責：

1. 接收藍芽（BLE）生理量測裝置（目前為 FORA 體溫計、血氧器，之後擴充其他品牌/量測項目）傳來的數值。
2. 暫存量測資料（接收時間、數值、上傳時間、上傳狀態）。
3. 定期透過 WiFi 將暫存資料上傳到指定 API。
4. 首次部署或需要換環境時，透過手機連上裝置熱點完成 WiFi 帳密、個案姓名/編號、個管師資訊等設定。

使用情境為醫療週邊裝置，強調 **24/7 穩定運作**、**資料不遺失**、**設定簡單**。

## 2. 硬體限制與關鍵設計決策

### 2.1 天線／無線共存

Pico W 使用的 CYW43439 是 WiFi + 藍牙合一晶片，共用同一根天線，晶片本身支援 coexistence（時間切片輪流使用），理論上 WiFi 與 BLE 可同時運作。但實務上：

- pico-sdk 上 BTstack（藍牙）與 lwIP（WiFi）同時常駐會共搶 RP2040 僅有的 264KB RAM 與 CPU。
- 「WiFi 熱點 + BLE 同時開」這種組合社群案例少、穩定度未經充分驗證，尤其對 24/7 醫療週邊裝置風險偏高。

**決策：同一時間只運作一種無線模式**，以狀態機切換，犧牲理論上的並行能力換取穩定性。這不是硬體強制限制，而是刻意的保守設計選擇。

### 2.2 模式切換輸入

Pico W 沒有板載使用者按鈕，只有 BOOTSEL。在不外接硬體的前提下：

- 讀取 BOOTSEL 需要暫時切換 QSPI_SS 腳位並關閉中斷，若在 BLE/WiFi 中斷正在運作時執行有當機風險（官方範例註解明載此限制）。
- **決策：只在開機後的短時間視窗（3~5 秒）輪詢 BOOTSEL，過了視窗之後在正常 24/7 運作期間完全不再輪詢**。使用者要進入熱點設定模式時，重新插拔電源並在開機瞬間按住 BOOTSEL。

### 2.3 板載 LED

Pico W 的 LED 接在 CYW43 晶片的 GPIO0，而非 RP2040 一般 GPIO，只能透過 `cyw43_arch_gpio_put()` 控制，且必須在主協作式迴圈中呼叫（不可在中斷處理常式中呼叫，社群已回報過在計時器 ISR 中呼叫、同時網路忙碌會導致 hang）。

## 3. 狀態機設計

```
開機
  │
  ▼
[BOOT_SELECT]  開機視窗 3~5 秒，LED 快閃，偵測 BOOTSEL 是否按住
  ├─ 按住 → [AP_CONFIG]
  └─ 未按住 → [BLE_RECEIVE]

[AP_CONFIG]   熱點設定模式，LED 常亮
  設定完成並儲存 → [BLE_RECEIVE]

[BLE_RECEIVE] 藍芽接收模式（預設常駐狀態）
  掃描/重連中 → LED 慢閃
  已連線、正常接收 → LED 心跳短閃
  收到資料 → 寫入 storage，重置 60 秒計時器
  60 秒無新資料 → [UPLOAD]

[UPLOAD]      上傳模式，LED 快閃
  連上 WiFi → 上傳所有待傳紀錄
  無論成功或失敗 → 一律回到 [BLE_RECEIVE]

任何模式下偵測到嚴重錯誤 → LED 三連閃+停頓，記錄錯誤狀態
```

切換到任何 WiFi 相關模式前，必須先關閉藍芽（`hci_power_control(HCI_POWER_OFF)`）；反之切回 BLE 前必須確保 WiFi/AP 已關閉。此「單一無線擁有者」規則由 `state_machine.c` 統一把關，避免日後改動時被意外破壞。

## 4. LED 燈號規範

| 狀態 | 燈號 | 時序 |
|---|---|---|
| 開機 BOOTSEL 偵測視窗 | 快閃 | 100ms on / 100ms off |
| 熱點設定模式 | 常亮 | — |
| BLE 接收－掃描/重連中 | 慢閃 | 1000ms on / 1000ms off |
| BLE 接收－已連線、正常接收 | 心跳短閃 | 每 2000ms 閃 50ms |
| 上傳模式 | 快閃 | 150ms on / 150ms off |
| 錯誤/異常 | 三連閃 + 停頓 | 3×(80ms on/off) 後停 1200ms，循環 |

## 5. 資料模型（初版）

- `device_config_t`：WiFi SSID / 密碼、個案姓名、個案編號、個管師資訊。
- `vital_record_t`：接收時間、量測類型、數值、上傳時間、上傳狀態。

## 6. 專案目錄結構

```
pico/
├── PROJECT_PLAN.md
├── README.md
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── lwipopts.h
├── btstack_config.h
└── src/
    ├── main.c
    ├── common.h
    ├── state_machine.c/.h
    ├── mode_boot_select.c/.h
    ├── mode_ap_config.c/.h
    ├── mode_ble_receive.c/.h
    ├── fora_protocol.c/.h       # TODO：實際 FORA GATT 協定
    ├── mode_upload.c/.h
    ├── upload_api.c/.h          # TODO：實際上傳 API 規格
    ├── led_status.c/.h
    └── storage.c/.h
```

## 7. 待補事項

1. ~~**FORA 藍芽 GATT 協定**~~ 已解決（2026-07-31，實機驗證）：真實協定跟一開始從官方標準文件假設的完全不同，詳見第 7.1 節。`fora_protocol.c` 已實作真實解析，實測能正確收到體溫數值。之後擴充血氧器或其他裝置，**不要**直接假設標準 profile 一定是真正資料來源，做法建議見第 7.1 節最後一段。
2. **上傳 API 規格**：endpoint、認證方式、JSON payload 格式、批次或單筆上傳。目前 `upload_api.c` 為 TODO stub。

### 7.1 FORA IR42 實際藍芽協定（重要，除錯過程完整記錄）

**一開始的錯誤假設**：用 nRF Connect 檢視 FORA IR42 時，看到它廣播標準 Bluetooth SIG **Health Thermometer Service (0x1809)** / `Temperature Measurement (0x2A1C)`（屬性 Indicate），一開始以為這就是資料來源，照標準 GATT Health Thermometer profile（IEEE-11073 32-bit FLOAT 格式）實作。結果連線、服務探索、特徵值探索、CCC 訂閱全部成功（`att_status=0x00`），但裝置從來不會真的送出 indication，最後都以 `HCI_EVENT_DISCONNECTION_COMPLETE reason=0x08`（Connection Timeout，連線在底層直接失聯，不是裝置主動乾淨斷線）收場。事後確認：0x1809 這個 service 雖然有宣告，但**不是實際資料傳輸的管道**。

**真正協定**（靠使用者找到的舊版已驗證可動的 MicroPython 實作反推出來，不是查文件查到的）：
- Service：`00001523-1212-efde-1523-785feabcd123` —— 其實是 Nordic nRF5 SDK 範例板內建的 "LED and Button Service"，FORA/Taidoc 借用它現成的雙向 pipe characteristic 來傳自己的封包格式，是常見的 OEM 做法。
- Characteristic：`00001524-1212-efde-1523-785feabcd123`，屬性 **Write + Notify**（不是 Indicate）。
- 流程：訂閱 Notify 後，**必須主動寫入觸發指令** `51 26 00 00 00 00 a3 1a`（write without response），裝置才會回傳目前量到的數值——單純訂閱不會自動推播，這是之前一直卡住的關鍵原因。
- 回應封包格式：第一個 byte 固定是 `0x51`；溫度 = `((byte[3]<<8 | byte[2]) & 0x0FFF) / 10.0`，單位攝氏。實測收到 `51 26 72 01 ea 00 a5 79` → 解析出 37.00°C，正確。

**行為細節**：
- FORA IR42 量測完後會**短暫廣播**一段時間（不是常駐廣播），且**要用主動掃描(active scan)才看得到裝置名稱**（名稱可能只在 scan response 封包裡，被動掃描看不到，`gap_set_scan_parameters()` 第一個參數要傳 1）。
- 每次連線只服務「連線 → 訂閱 → 觸發 → 收一筆資料 → （我們主動）斷線」這種短暫請求/回應循環，不會維持長連線持續推播。裝置的 GATT attribute table 在多次連線間固定，`mode_ble_receive.c` 會在第一次連線探索完後快取 handle，之後重連直接跳過探索，盡量在裝置自己的連線時間窗內完成。
- Manufacturer Specific Data（`00 00 c0 26 da 28 b6 e6`）只是裝置自己 MAC 位址的重複，**不含溫度數值**，已用多次量測比對排除這個可能性。
- 因為是短暫連線模式，`mode_ble_receive_run()` 的 60 秒無新資料計時器改成「不管目前是否連線中，只看距離上次成功收到資料多久」，不能只在連線中才檢查（已修正，否則計時器幾乎不會被正確觸發）。

**擴充其他裝置時的啟示**：不能只看裝置廣播/宣告的標準 service 就假設那是真正資料來源，尤其是 OEM/白牌醫療裝置，很可能把資料塞進看起來無關的自訂 characteristic（甚至是別家 SDK 範例板留下的 demo service）。之後遇到新裝置，最快的驗證方式：先找有沒有現成能動的參考實作（哪怕是舊版、其他語言，例如這次找到的舊 MicroPython 版），沒有的話才用 nRF Connect 邊連線邊觸發裝置量測、比對 raw bytes 反推格式。

3. ~~**開發環境**~~ 已完成（2026-07-31）：pico-sdk v2.3.0、ARM 工具鏈、CMake、Ninja、MinGW（host 端編譯 pioasm/picotool 用）均已安裝並驗證可編譯燒錄，韌體已在實機上跑起來、確認 BLE 掃描與 USB 序列 log 正常。
4. **待傳生理資料的持久化**：骨架階段 `storage.c` 只把待傳紀錄放在 RAM 環狀陣列（裝置設定則確實寫入 flash），原因是要正確手刻一個會被 24/7 連續寫入、跨 flash page 的 ring buffer，在沒有實機可編譯測試的情況下風險偏高，容易寫出「看起來合理但實際會壞資料」的程式碼。目前設計下，正常流程裡待傳資料在裝置手上的時間最多略多於 60 秒（BLE接收→上傳→清空），影響範圍是「上傳前意外斷電/重開機」才會遺失資料。正式量產前務必評估：(a) 改用 littlefs 做 wear-leveling 的持久化 ring buffer，或 (b) 至少把待傳資料落地成簡單的 flash 多 sector 輪替寫入，並在實機上驗證。
5. **擴充其他廠牌裝置**：`fora_protocol.c` 之後可抽成通用 `ble_device_driver` 介面，讓 `mode_ble_receive.c` 依裝置類型分派解析器，目前先以單一 FORA 實作驗證整體流程可行。
6. ~~**`dhcpserver.c/.h`、`dnsserver.c/.h`**~~ 已完成（2026-07-31）：這兩組 pico-examples 共用工具程式（來源 `pico-examples/pico_w/wifi/access_point_wifi_provisioning/{dhcpserver,dnsserver}/`）已複製進專案根目錄並加進 `CMakeLists.txt` 的編譯來源，`mode_ap_config.c` 的相依已補齊。但整個熱點設定模式流程本身尚未實機測試過。
7. **熱點設定網頁**：`mode_ap_config.c` 用 lwIP raw TCP API 手刻一個極簡單的單連線 HTTP server（而非官方範例的 lwIP httpd + `makefsdata` codegen），換取骨架階段不需要額外建置工具鏈依賴；之後若要做更完整的設定頁（例如即時回顯目前設定值）可再評估要不要改回 httpd。
8. **同一次量測重複記錄**：FORA IR42 量測後會持續廣播一段時間，目前 `mode_ble_receive.c` 每次掃到符合的廣播就會重新連線、送觸發指令，同一次量測可能因此被重複連線好幾次、`storage_append_record()` 被呼叫多次寫入完全相同的數值。需要加防重複機制，例如：記住最近一次已成功讀取的數值+時間，短時間內（例如 30~60 秒）看到同一顆裝置的廣播就不再重新連線；或是讀完之後暫停對該裝置的掃描比對一段時間。

## 8. 里程碑（建議順序）

1. ✅ 環境就緒：VSCode + 擴充套件 + SDK/工具鏈，能編譯燒錄基本韌體。
2. ✅ 骨架就緒：狀態機 + LED + BOOTSEL 開機視窗，已在實機驗證運作。
3. ⬜ 熱點設定模式：程式碼已寫好，尚未實機驗證手機連線、表單送出、設定值寫入 flash 並可讀回。
4. ✅ BLE 接收模式：已用真實 FORA IR42 協定驗證能連線、收值、寫入 storage，實測收到 37.00°C 正確數值。剩下待辦：防重複記錄（見第 7 節第 8 點）。
5. ⬜ 上傳模式：等實際 API 規格才能實作，驗證 60 秒觸發、上傳成功/失敗都能正確回到 BLE 接收模式。
6. ⬜ 24 小時連續運作測試，觀察 flash 抹寫、記憶體、藍芽穩定度。
