# Raspberry Pi Pico W 生理訊號中繼裝置 — 專案計畫

> 狀態總覽與交接文件（2026-08-06 整理）。內容跟程式碼有衝突時一律以程式碼為準，並回頭更新這份文件。

## 目前狀態摘要

**已完成、已實機驗證：**

- 環境、骨架（狀態機、BOOTSEL 開機視窗、LED 燈號、flash 儲存）。
- 熱點設定模式（AP_CONFIG）：手機連上熱點自動跳出設定頁（captive portal），可掃描附近 WiFi、填個案資訊，會帶入既有設定值。
- BLE 接收三種 FORA 裝置（額溫槍、血氧計、血壓計）並正確上傳，見第 6 節協定細節。
- WiFi 連線 + HTTPS 上傳，含失敗自動重試（佇列持久化在 flash，斷電不遺失）、NTP 校時。
- 電子紙顯示器（Waveshare Pico-ePaper-2.9）Phase 1+2：四種模式的畫面都燒錄驗證過，見第 12 節。

**已寫完程式碼、編譯過關，但還沒燒錄/實機測試（2026-08-06 裝置不在身邊期間完成，見第 8.5 節）：**

- 血壓計解析加上記錄類型旗標檢查＋血糖協定實作（見第 6.3/6.4 節）。
- 血壓計時鐘合理性檢查（跟 NTP 比對，不合理就退回用 Pico 收到時間，容許誤差 7 天**待確認**，見第 7.3 節）。
- 上傳伺服器網址／認證金鑰改成可透過 AP_CONFIG 設定、TLS 憑證驗證框架（等正式後端網址才能真的填憑證/收緊驗證）。
- AP 熱點密碼、SSID 都改成每台裝置唯一衍生（不再寫死）。
- 已上傳紀錄保留機制（主持人要求，保留最近 200 筆，數字**待確認**，見第 5/7.3 節）。

**曾經做過又移除的：** 電子紙 Phase 3 局部刷新——重新檢視後發現沒有實測觀察到的具體場景真的需要它，換不到的好處不值得承擔未測試的風險，決定拿掉、維持只用全刷（見第 12.5 節）。

**還沒做完/還沒驗證，見第 7 節完整清單（含第 7.3 節「待與相關人員確認事項」——這幾項需要外部決策，不是我能自己判斷定案的）：**

- 24 小時等級耐用性測試完全沒做過——這是這次交接的主要任務。
- WiFi QR code 手機掃碼、心跳時間戳視覺驗證、上面那批新程式碼的實機測試都還沒做，見第 7.2 節。

## 0. 在新電腦上接續開發

1. `git clone` 這個 repo（原始碼、這份文件都在 git 裡；**開發環境本身不在 git 裡**，換機器要重裝）。
2. 安裝方式二選一：
   - 裝 VSCode 官方 **"Raspberry Pi Pico"** 擴充套件，跑一次 **Import Pico Project** 指向這個資料夾，讓它自動下載 SDK/工具鏈。
   - 手動安裝：`winget install Kitware.CMake`、`winget install Ninja-build.Ninja`、`winget install Arm.GnuArmEmbeddedToolchain`、`winget install BrechtSanders.WinLibs.POSIX.UCRT`（host 端編譯 pioasm/picotool 需要），再 `git clone --branch 2.3.0 --depth 1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git`，並把路徑設成使用者環境變數 `PICO_SDK_PATH`。
3. **已知的 PowerShell 環境變數問題**：Claude Code／某些終端機工具開的新視窗可能繼承到舊的環境變數快取，`cmake`/`ninja`/`arm-none-eabi-gcc` 會找不到。如果遇到這種情況，在該次終端機視窗手動把下面幾個路徑加到 `$env:Path` 開頭，並設 `$env:PICO_SDK_PATH`：
   - `C:\Program Files\CMake\bin`
   - `$env:LOCALAPPDATA\Microsoft\WinGet\Links`
   - `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin`
   - `C:\Users\WunKong\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin`（WinLibs MinGW，**這個容易被漏掉**，漏了會導致編譯最後一步呼叫 `picotool.exe` 轉 `.uf2` 失敗，錯誤訊息是 `STATUS_DLL_NOT_FOUND`）
   一般開新的終端機視窗（不是這個工具開的）或重開機就不會有這個問題。
4. 編譯：`cmake -S . -B build -G Ninja` 然後 `cmake --build build`。
5. 燒錄：**這台機器上本機建置的 `picotool.exe` 沒有 `libusb-1.0.dll`，`picotool reboot -u -f` 這種遠端觸發重開機的指令用不了**。改用手動方式：裝置接電腦時按住 BOOTSEL（或已在跑舊韌體時直接按住 BOOTSEL 再重新插拔電源），等 `RPI-RP2` 磁碟機出現，把 `build\pico_gateway.uf2` 複製過去即可（複製完裝置會自動重開機進新韌體）。
6. 除錯／看 log：裝置正常開機後會出現一個 USB CDC 序列埠（例如 `COM10`，實際編號依電腦而定），baud rate 115200。可以用 VSCode 的 **Serial Monitor** 擴充套件，或任何序列埠工具。**注意**：Windows 的 `.NET SerialPort` 類別預設不會 assert DTR，Pico 的 TinyUSB CDC 會因此完全不輸出任何資料——如果自己寫監看小工具，`Open()` 之後要記得設 `DtrEnable = $true; RtsEnable = $true`。

## 1. 產品概述

一台以 Raspberry Pi Pico W 為核心的中繼裝置，安裝於個案床邊或隨身攜帶，負責：

1. 接收藍芽（BLE）生理量測裝置（目前：FORA IR42 額溫槍、FORA O2 血氧計、FORA D40 血壓血糖二合一計）傳來的數值。
2. 暫存量測資料（接收時間、數值、上傳時間、上傳狀態），存進 flash，斷電不遺失。
3. 定期透過 WiFi 將暫存資料上傳到指定 API，會嘗試用 NTP 把時間戳換算成真實世界時間。
4. 首次部署或需要換環境時，透過手機連上裝置自己的獨立熱點（跟裝置實際要連的目標 WiFi 是兩個完全不同的網路）完成 WiFi 帳密、個案姓名/編號、個管師資訊等設定；重新設定時會帶入目前已存的值。

使用情境為醫療週邊裝置，強調 **24/7 穩定運作**、**資料不遺失**、**設定簡單**。

## 2. 硬體限制與關鍵設計決策

### 2.1 天線／無線共存

Pico W 使用的 CYW43439 是 WiFi + 藍牙合一晶片，共用同一根天線。**決策：同一時間只運作一種無線模式**（BLE 接收 / WiFi 熱點設定 / WiFi 上傳三選一），以狀態機切換，犧牲理論上的並行能力換取穩定性。這不是硬體強制限制，是刻意的保守設計選擇，由 `state_machine.c` 統一把關。

### 2.2 模式切換輸入

Pico W 沒有板載使用者按鈕，只有 BOOTSEL。**決策：只在開機後的短時間視窗（約 4 秒）輪詢 BOOTSEL，過了視窗之後在正常 24/7 運作期間完全不再輪詢**（讀取 BOOTSEL 需要暫時切換 QSPI_SS 腳位並關閉中斷，正常運作期間執行有當機風險）。使用者要進入熱點設定模式時，重新插拔電源並在開機瞬間按住 BOOTSEL。

**重要區分（測試時常搞混）**：
- **BOOTSEL 開機瞬間按住** → 韌體正常開機、`mode_boot_select_check()` 偵測到、進入 AP_CONFIG 熱點設定模式。這是軟體邏輯。
- **RP2040 boot ROM 層級的 BOOTSEL**（開機那一刻電源腳位供電前就按住/插入電源時按住不放） → 裝置完全不會執行任何韌體，變成 `RPI-RP2` USB 隨身碟（燒錄模式）。這是晶片硬體行為，跟上面那個完全無關。

### 2.3 板載 LED

Pico W 的 LED 接在 CYW43 晶片的 GPIO0，只能透過 `cyw43_arch_gpio_put()` 控制，且必須在主協作式迴圈中呼叫（不可在中斷處理常式中呼叫）。

## 3. 狀態機設計

```
開機
  │
  ▼
[BOOT_SELECT]  開機視窗約 4 秒，LED 快閃，偵測 BOOTSEL 是否按住
  ├─ 按住 → [AP_CONFIG]
  └─ 未按住 → [BLE_RECEIVE]

[AP_CONFIG]   熱點設定模式，LED 常亮
  設定完成並儲存 → [BLE_RECEIVE]

[BLE_RECEIVE] 藍芽接收模式（預設常駐狀態）
  掃描/重連中 → LED 慢閃
  已連線、正常接收 → LED 心跳短閃
  收到任一裝置的資料 → 寫入 storage（同時落地 flash，經過判重，見第 6 節）
  收到第一筆資料之後，開始倒數：距離「最後一筆資料」超過設定的秒數沒有新資料
    且待傳佇列非空 → [UPLOAD]
  （在收到任何資料之前不會倒數；判重把資料濾掉、佇列仍是空的話也不會切換，
   避免血壓計那種要 30~45 秒才會推播一次的裝置被提早打斷，也避免白跑一趟
   WiFi 只為了確認「沒有東西要傳」）

[UPLOAD]      上傳模式，LED 快閃
  連上 WiFi → 嘗試 NTP 校時 → 上傳所有待傳紀錄
  無論成功或失敗 → 一律回到 [BLE_RECEIVE]（失敗的紀錄留著，下次會重試）

任何模式下偵測到嚴重錯誤 → LED 三連閃+停頓，記錄錯誤狀態
```

切換到任何 WiFi 相關模式前必須先關閉藍芽，反之亦然，此「單一無線擁有者」規則由 `state_machine.c` 統一把關。

`BLE_IDLE_UPLOAD_TRIGGER_MS`（`state_machine.c`）目前 **5 秒**：收到任一裝置的第一筆資料後，5 秒內沒有新資料、且待傳佇列非空就切到 UPLOAD。

裝置重新連線冷卻時間 `DEVICE_RECONNECT_COOLDOWN_MS`（`mode_ble_receive.c`，依裝置種類分開設定，**跨 BLE_RECEIVE/UPLOAD 模式切換持續有效**）：某種裝置拿到讀值之後，這段時間內不會再重新連線同一種裝置，就算裝置還在廣播也不理它，讓它有機會真的休眠/閒置。**這幾個值存在的唯一理由就是要讓裝置真正撐到自己的休眠門檻**，跟判重的正確性無關（判重靠時間戳/數值比對，見第 6 節），設計原則是在「裝置能不能真的休眠」跟「多久能抓到真正的新量測」之間找平衡，優先滿足前者——只要夠長讓裝置真的睡著就好，設更長不影響正確性，只是新量測會晚一點被抓到。

| 裝置 | 冷卻時間 | 官方休眠門檻（2026-08-06 使用者確認） | 說明 |
|---|---|---|---|
| 額溫槍 | 60 秒 | 1 分鐘 | 冷卻等於門檻，沒有額外餘裕 |
| 血氧計 | 5 秒 | **未知，待確認**（見第 7 節待確認事項） | 目前是暫定值，不是根據官方休眠時間設定的 |
| 血壓計 | 4 分鐘 | 3 分鐘 | 多留 1 分鐘餘裕，避免剛好卡在門檻邊緣被提早重連 |

## 4. LED 燈號規範

| 狀態 | 燈號 | 時序 |
|---|---|---|
| 開機 BOOTSEL 偵測視窗 | 快閃 | 100ms on / 100ms off |
| 熱點設定模式 | 常亮 | — |
| BLE 接收－掃描/重連中 | 慢閃 | 1000ms on / 1000ms off |
| BLE 接收－已連線、正常接收 | 心跳短閃 | 每 2000ms 閃 50ms |
| 上傳模式 | 快閃 | 150ms on / 150ms off |
| 錯誤/異常 | 三連閃 + 停頓 | 3×(80ms on/off) 後停 1200ms，循環 |

## 5. 資料模型

- `device_config_t`：WiFi SSID / 密碼、個案姓名、個案編號、個管師資訊、上傳伺服器網址、上傳認證金鑰（見第 8.5 節）。
- `vital_type_t`：`UNKNOWN`(0) / `TEMPERATURE`(1) / `SPO2`(2) / `PULSE_RATE`(3) / `SYSTOLIC`(4) / `DIASTOLIC`(5) / `GLUCOSE`(6，協定已確認並實作，見 6.4 節，尚未實機驗證)。
- `vital_record_t`：`received_at_ms`（Pico 收到時間，或裝置有自己的量測時間戳時會被那個值取代）、`vital_type_t`、數值、上傳時間、上傳狀態，以及：
  - `device_measured_key`：裝置自己認證過的量測時間戳（分鐘解析度，`fora_protocol.c` 編碼/解碼），0 代表這種裝置沒有這個資訊。判重優先用這個值，見第 6 節。
  - `source_kind`：是哪種裝置回報的（`fora_device_kind_t`，用不透明的 `uint8_t` 存，避免 `common.h` 依賴 `fora_protocol.h`）。用途：避免共用同一個 `vital_type_t` 的不同裝置（例如脈搏同時來自血氧計跟血壓計）互相污染彼此的判重結果，見第 6 節。
- `LOCAL_UTC_OFFSET_SEC`（`common.h`）：專案目前只在台灣用，統一假設本地時間是 UTC+8，`display_status.c`（NTP 校時換算）跟 `fora_protocol.c`（裝置自己時鐘的量測時間換算）共用同一份常數。

**已上傳紀錄保留機制（2026-08-06 加做，尚未實機測試，主持人明確提出的需求）**：原本 `storage_mark_uploaded()` 上傳成功的紀錄會直接從待傳佇列移除、不再保留在任何地方。**主持人不希望上傳完後本機資料被直接清除，要求至少保留紀錄**。修法：新增一個獨立的環狀緩衝（`storage.c` 的 `s_upload_history[]`／`storage_get_upload_history()`），保留**最近 200 筆**已上傳成功的紀錄，一樣持久化在 flash（待傳佇列前面再保留幾個 sector），滿了就覆蓋最舊的一筆，不會無限成長。**200 這個數字是概略估計，不是主持人指定的精確值**：抓的假設是單一個案就算四種生理值都密集量測（一天合計約 20 筆讀值），200 筆大約涵蓋 1~2 週份量，在「保留多少歷史」跟「flash 空間/磨損」之間找一個折衷——這個假設本身沒有實際使用數據驗證過，如果之後發現實際量測頻率跟這裡的估計差很多，應該回來調整。目前**還沒有任何介面可以讀出這份歷史**（沒有畫面顯示、沒有上傳/匯出機制），只是先把資料保留下來，之後如果需要查驗可以再加讀取介面（例如序列埠指令、或另開一個除錯用的 HTTP endpoint）。

## 6. 支援的裝置與藍牙協定（重要，之後擴充新裝置前必讀）

三種裝置都用裝置廣播名稱含 `"FORA"` 判斷是不是要連線的裝置，再用名稱裡的其他字元判斷是哪一種型號（見 `fora_protocol_matches_advertisement()`）。**這個判斷邏輯很脆弱**：如果之後買到名稱不含這些關鍵字的新裝置，或現有裝置改款換了廣播名稱，需要回來調整。

### 6.1 FORA IR42 額溫槍（`FORA_DEVICE_THERMOMETER`，kind=1）

- 判斷依據：名稱含 `"FORA"`，且不含 `"O2"`／`"D40"`（預設落在這一種）。
- **不是標準 Bluetooth SIG Health Thermometer Service**（雖然也宣告 0x1809，但那個不是真正資料管道，連線/訂閱都會成功但永遠收不到資料）。
- 實際協定：借用 Nordic nRF5 SDK 範例板的 "LED and Button Service"（FORA/Taidoc 常見 OEM 做法）：
  - Service：`00001523-1212-efde-1523-785feabcd123`
  - Characteristic：`00001524-1212-efde-1523-785feabcd123`（Write + **Notify**）
  - 流程：訂閱 Notify 後，**必須主動寫入觸發指令** `51 26 00 00 00 00 a3 1a`（write without response），裝置才會回傳目前量到的數值。
  - 回應格式：`byte[0]==0x51` 才是有效回應；溫度 = `((byte[3]<<8 | byte[2]) & 0x0FFF) / 10.0`，單位攝氏。實測範例：`51 26 72 01 0e 01 a5 9e` → 37.00°C。

### 6.2 FORA O2 血氧計（`FORA_DEVICE_OXIMETER`，kind=2）

- 判斷依據：名稱含 `"O2"`（實測裝置名稱是 `"FORA O2"`）。
- 跟額溫槍用完全相同的 Service/Characteristic UUID（同一套 Nordic LED/Button Service pipe），但兩種裝置各自的 GATT attribute table 排列不同，**handle 不通用**（依裝置種類分開快取，見第 8 節）。
- 訂閱、觸發指令跟額溫槍完全一樣（同一個 `FORA_TRIGGER_COMMAND`）。
- 回應格式：`byte[0]==0x51`；
  - SpO2 = `(byte[3]<<8 | byte[2]) & 0x0FFF`，單位 %，**不用除以 10**（跟溫度的公式不同）。
  - 脈搏 = `byte[5]`，單位 bpm，直接就是整數。
  - `byte[4]`／`byte[6]`／`byte[7]` 用途未知，目前忽略。
  - 實測範例：`51 26 61 00 3c 4c a5 05` → SpO2=97%、脈搏≈76。

### 6.3 FORA D40 血壓計（`FORA_DEVICE_BLOOD_PRESSURE`，kind=3）

- 判斷依據：名稱含 `"D40"`（實測裝置名稱是 `"FORA D40"`）。
- 跟額溫槍/血氧計走**完全相同**的 Nordic LED/Button Service 自訂 pipe，**不是**標準 Bluetooth SIG Blood Pressure Service。真正協定是靠反編譯官方 Windows 程式 `BLE_PCLink_Library.dll`（`TaiDoc.BLE_PcLink` 命名空間）才確認的，反推方法見第 8 節；曾經誤以為走標準規格訂閱 `0x2A35` Indicate，實測連線後等了 76 秒都等不到推播——根因是裝置量測完才開始廣播，連線建立時量測早就結束，Indicate 只推播訂閱後的新事件，這是協定層面的限制，不是等待不夠久。
- 連線後**需要先配對**（`sm_request_pairing()`）才能訂閱/寫入成功，額溫槍/血氧計不需要（原因未深究，推測是這個 characteristic 多加了 Security Mode 1 Level 2 要求）。
- **取得「目前這一筆」記錄的流程**：訂閱 Notify 後主動送兩次指令（三種裝置共用的指令格式：`{0x51, cmd, p1, p2, p3, p4, 0xA3, checksum}`，checksum = 前 7 bytes 總和的低位元組，`fora_protocol_build_command()`）：
  1. 送 `cmd=0x25`（`FORA_BP_CMD_GET_RECORD_PART_A`，索引 0=最新一筆）→ 回應取 `byte[2..5]`
  2. 送 `cmd=0x26`（`FORA_BP_CMD_GET_RECORD_PART_B`）→ 回應取 `byte[2..5]`
  3. 兩次各 4 bytes 接成 8 bytes，交給 `fora_protocol_parse_reading(FORA_DEVICE_BLOOD_PRESSURE, ...)` 解析。
  - 想問裝置目前有幾筆記錄：`cmd=0x2B`（`FORA_BP_CMD_GET_RECORD_COUNT`），目前用不到，只記錄協定。
- **8 bytes 私有格式**（已對照官方反編譯原始碼的 `BloodPressure` class 逐位元確認無誤）：

  | Byte | 內容 |
  |---|---|
  | `byte[0]` | day (bits0-4) \| month 低 3 bit (bits5-7) |
  | `byte[1]` | month 最高 1 bit (bit0) \| year_offset (bits1-7，year = +2000) |
  | `byte[2]` | minute (bits0-5) \| 心律不整旗標 (bit6) \| **記錄類型旗標 (bit7：0=血糖、1=血壓，見 6.4 節)** |
  | `byte[3]` | hour (bits0-4) \| IHB 狀態 (bits5-6) \| 是否為平均值 (bit7) |
  | `byte[4]` | 收縮壓（整數 mmHg） |
  | `byte[5]` | 平均壓（目前不取） |
  | `byte[6]` | 舒張壓（整數 mmHg） |
  | `byte[7]` | 脈搏（整數 bpm） |

  `byte[0..3]` 的日期/時分同時也是裝置自己認證過的量測時間戳，`fora_protocol_decode_measured_key()` 解碼成一個可比較的鍵值，`fora_protocol_measured_key_to_datetime()`/`_to_epoch_ms()` 還原成日期或 epoch ms，供判重、畫面顯示、上傳時間戳使用（見下面「判重」跟第 8 節）。

- **`byte[2]` bit7 記錄類型旗標（2026-08-06 已修好，見第 8.5 節第 21 點）**：這台裝置是血壓血糖二合一，「問目前這一筆記錄」這組指令回傳的**可能是血壓、也可能是血糖**，靠 `byte[2]` 的 bit7 分辨（官方程式 `GenBgmAndBpmMeter.GetRecord()` 就是靠這個 bit 決定要 new 哪一種 record class）。`fora_protocol_parse_reading()` 原本完全沒有檢查這個 bit，無條件把回應當成血壓資料解析，如果使用者上次量的其實是血糖，會把血糖的 `byte[4..7]` 誤當成血壓的 `byte[4]/[6]/[7]` 硬解，產生看起來合理但完全錯誤的血壓數值，且不會有任何錯誤訊息——**這個 bug 現在已經修好**，解析前會先看這個 bit 分流到第 6.4 節的血糖格式，**但這個修法還沒有實機測試過**，見第 7.2 節第 11 點。
- **裝置不會標記「已讀」、會持續廣播很久**：每次連線都回傳一模一樣的「目前最新一筆」記錄。判重邏輯：跟同類型（`vital_type_t`）且同一種裝置（`source_kind`）回報的最後讀值比對——雙方都有 `device_measured_key` 就直接比對是否相等（同一個時間戳保證是同一筆記錄，不用猜時間窗口）；沒有這個資訊的裝置（額溫槍/血氧計）才退回用「數值相同+10 分鐘內」的經驗法則。已修好且實機驗證：同一筆舊記錄重複收到時不會重複上傳；量到真正新的血壓值時能正確被當成新資料。
- **已完整實機驗證**：連線→配對→兩段式取記錄→解析→上傳，數值跟裝置螢幕一致；判重、裝置時間戳換算（畫面顯示跟上傳都優先用裝置自己的量測時間，不是 Pico 收到 BLE 通知的時間，即使 NTP 校時失敗也不受影響）都已驗證過，細節見第 8 節修復記錄。

### 6.4 FORA D40 血糖計部分（協定已反推確認並實作，2026-08-06，尚未實機驗證）

- **走的是跟血壓計完全相同的指令/資料管道**：同一個 Service/Characteristic、同一組 `cmd=0x25`+`cmd=0x26` 兩段式取記錄指令、同一套 8-byte 組合方式。裝置端把「目前最新一筆記錄」當成單一個概念維護，可能是血壓、也可能是血糖，靠回應 `byte[2]` 的 bit7 分辨（見第 6.3 節）——血糖支援沒有新增任何 BLE 指令邏輯，`fora_protocol_parse_reading()` 收到組好的 8 bytes 後先看類型旗標，分流到血糖或血壓兩條解析路徑（見第 8.5 節第 21 點）。
- **反推來源**：反編譯同一支官方程式 `BLE_PCLink_Library.dll` 裡的 `BloodGlucose2in1` class（`GenBgmAndBpmMeter.GetRecord()` 在 bit7==0 時會 new 這個 class），跟血壓計的反推方法、工具鏈相同，見第 8 節。**這個協定目前只有比對官方反編譯原始碼，還沒有實機量測比對過**（沒有 D40 的血糖試紙/血液樣本可以實際測試），下方欄位說明如果之後實作時發現跟真實裝置對不上，以實機為準。
- **8 bytes 私有格式**（`byte[0..3]` 跟血壓計共用同一套日期/時間編碼）：

  | Byte | 內容 |
  |---|---|
  | `byte[0]` | day (bits0-4) \| month 低 3 bit (bits5-7)，跟血壓計相同 |
  | `byte[1]` | month 最高 1 bit (bit0) \| year_offset (bits1-7)，跟血壓計相同 |
  | `byte[2]` | minute (bits0-5) \| 記錄類型旗標 (bit7，這裡應該是 0) |
  | `byte[3]` | hour (bits0-4) |
  | `byte[4]`, `byte[5]` | 血糖值，16-bit 小端：`glucose = byte[5]*256 + byte[4]`，單位 mg/dL（官方 `GlucoseUnitEnum.mgdL` 是預設/數值 0） |
  | `byte[6]` | ambient（環境溫度，官方程式沒有實際使用這個欄位做判斷，可以先忽略） |
  | `byte[7]` | codeNo (bits0-5, `&0x3F`) \| 測試時機 (bits6-7，`(byte[7]&0xC0)/64`：0=一般、1=飯前(AC)、2=飯後(PC)、3=品管(QC)) |

  官方程式的合理性檢查：`glucose == 65535` 或 `glucose == 255` 時視為無效讀值（`Invalid`），不是真正的血糖數字。
- **已實作（2026-08-06，尚未實機驗證）**：`fora_protocol_parse_reading(FORA_DEVICE_BLOOD_PRESSURE, ...)` 收到組好的 8 bytes 後會先檢查 `byte[2] & 0x80`，0 就照上表解析血糖、回傳 `VITAL_TYPE_GLUCOSE`，非 0 才照原本的血壓邏輯解析；`mode_ble_receive.c` 原本「送 part A/B、組 8 bytes」的流程不用改，分流是在解析階段做的。畫面（`display_status.c` 的血糖列）、上傳（`upload_api.c`／`test_server/app.py` 的 `VITAL_TYPE_NAMES`）都已經能正確顯示/傳送這個類型，不需要額外改動。**還沒有實機量過一次血糖比對裝置螢幕數字驗證上表欄位正確**，見第 7.2 節第 11 點。

## 7. 待辦事項

分成兩類：**7.1 軟體層面**是需要寫新程式碼/改邏輯的（功能還沒做完或有已知的正確性風險），**7.2 測試/驗證層面**是程式碼已經寫好、需要實際跑一遍確認正確的。兩者優先順序不互相排斥，可以平行進行。

### 7.1 軟體層面待辦（需要寫新程式碼／改邏輯）

**2026-08-06 更新：這一節列出的 7 項當時都還沒寫，這次裝置不在身邊沒辦法實機操作，趁機把全部 7 項的程式碼都寫完、編譯過關了，細節見第 8.5 節。全部還沒燒錄、更沒有實機測試過，下面只留還沒寫程式碼、或者程式碼寫完但還需要真正決定值/憑證才能算完成的項目：**

1. **TLS 憑證驗證框架已接好，但還沒有真正的憑證可以放**：見 `upload_tls_ca_cert.h`，`UPLOAD_CA_CERT_PEM` 目前是空字串，upload_api.c 會照舊用不驗證模式（並在 log 印警告）。等正式後端網址確定之後，把該伺服器的憑證/CA PEM 貼進這個檔案就會自動改成真正驗證，不用改任何其他程式碼——**這個檔案本身沒有東西要寫了，純粹是在等一個「正式後端網址」的決定**。
2. **上傳伺服器網址/認證金鑰目前用 AP_CONFIG 表單設定，但伺服器端還沒有實作認證檢查**：`upload_api.c` 已經會在有填認證金鑰時送出 `X-API-Key` 標頭，但這是單邊的——目前唯一的測試伺服器 `test_server/app.py` 完全沒有檢查這個標頭（也不應該檢查，它就是設計給沒有認證的本機測試用）。正式後端要自己實作驗證這個標頭的邏輯。
3. 見第 9 節「已知限制」剩下還沒有處理、且不是純軟體能解決的項目（例如 flash wear-leveling 需要評估要不要換成 littlefs，是比較大的改動，這次沒有一起做）。

### 7.2 測試/驗證層面待辦（功能已經寫好，需要實機測試確認正確）

1. ✅ 血壓計完整「BLE 收到 → WiFi 上傳」流程：連線→配對→兩段式取記錄→解析→上傳，數值跟裝置螢幕一致。
2. ✅ 血壓重複上傳同一筆記錄：改用裝置量測時間戳判重＋血壓計冷卻拉長到 4 分鐘，同一筆舊記錄不會重複上傳、真正新的量測能正確被當成新資料。
3. ✅ **~~血壓計 4 分鐘冷卻是否真的讓裝置自動關機~~（2026-08-05 晚間約 19:30 燒錄的版本已實機確認）**：使用者確認那個版本（血壓計冷卻 4 分鐘）裝置真的會自動休眠，拉長冷卻時間讓裝置撐過 3 分鐘門檻的假說成立。
4. **NTP 校時成功率／定期重新校時邏輯**：已修過「成功一次就永遠跳過重新校時」的正確性 bug，改成每 6 小時最多真的打一次網路（見 `wall_clock.c` 的 `NTP_RESYNC_INTERVAL_MS`）。**還沒實機測試過**：需要確認第一次開機時仍然能正常校時成功、且連續運作超過 6 小時後真的會觸發一次重新校時。
5. **24 小時等級耐用性測試**：這是這次交接的主要目的。讓裝置長時間連續運作（例如放床邊，斷續有真實裝置量測、斷續上傳），觀察：
   - 是否會卡死、無回應（需要重新插電才會恢復）。
   - flash 寫入次數多了之後是否穩定（沒有 wear-leveling，見第 9 節）。
   - WiFi 連線成功率、是否會遇到偶發性的 `NONET` 快速失敗（測試過程中觀察到過，原因未深入排查）。
   - 斷電測試：量測後、還沒上傳成功前故意拔電源，重開機後確認待傳資料還在，且會在下次連上 WiFi 時重試上傳。
6. **多裝置情境**：目前只驗證過「一次一種裝置在旁邊」，沒測過三種裝置同時在附近廣播、輪流量測的情境（連線順序、掃描優先權、是否會互相干擾）。
7. **不同 WiFi 環境**：目前主要測試環境是家用路由器（WPA2）跟 iPhone 個人熱點。實際部署環境（醫院/病患家中）的路由器認證模式可能不同，程式碼已經做了多種認證模式輪流嘗試（見 `mode_upload.c` 的 `WIFI_AUTH_MODES_TO_TRY`），但沒有大量現場測試過。
8. **WiFi QR code 手機掃碼測試**：見第 12.7 節，程式碼已寫完、編譯過關，但還沒有真的拿手機掃碼確認會跳出「加入 WiFi」的系統提示。
9. **「Scanning」心跳時間戳的畫面視覺驗證**：見第 12.6 節，邏輯已經審視過、序列埠 log 能間接佐證運作時機，但還沒有真的空出一段裝置都不在旁邊廣播、超過 180 秒的安靜時間，肉眼確認面板上的時間戳有沒有正確前進。
10. **電子紙連續讀值即時更新／24 小時強制刷新驗證**：還沒確認 BLE_RECEIVE 連續收到好幾筆讀值時畫面是不是每次都有即時更新，也還沒驗證過 `MAX_REFRESH_INTERVAL_MS`（24 小時強制刷新）的邏輯有沒有真的觸發過（可以先改成短時間測試）。
11. **【新增】血糖協定實機驗證**：`fora_protocol.c` 已經接上血糖解析（見第 6.4/8.5 節），但這個格式只比對過官方反編譯原始碼，還沒有拿真的 D40 量過一次血糖比對裝置螢幕數字——量測時機的判斷（`byte[2]` bit7）、血糖值換算、無效值判斷都需要實機確認。
12. **【新增】血壓計時鐘合理性檢查實機驗證**：`mode_upload.c` 已經接上「跟 Pico 現在時間比對，差距過大就不信任裝置時鐘」的邏輯（見第 8.5 節），還沒有實機測試過兩種情境：(a) 裝置時鐘正常時是否還是正確採用裝置時間戳；(b) 刻意製造裝置時鐘異常（如果測試裝置支援改時間）時是否正確退回 Pico 收到時間。
13. **【新增】AP 熱點密碼改成每台裝置唯一衍生**：`mode_ap_config.c` 已經改成從 RP2040 board ID 衍生密碼（見第 8.5 節），還沒有實機確認電子紙螢幕上顯示的密碼、跟手機實際連線需要輸入的密碼是同一組。
14. ~~電子紙局部刷新（Phase 3）實機驗證~~**（2026-08-06 已移除，不再需要驗證，見第 12.5 節）**：局部刷新曾經接上又拿掉了——沒有任何實測觀察到的具體場景真的需要它，換來的是沒有實機驗證過的呼叫順序風險跟疊代殘影需要的額外清理邏輯，為一個從未真正發生過問題的場景背負未測試的複雜度不值得，決定只保留全刷。

### 7.3 待與相關人員確認事項（需要外部決策，不是純技術問題，我不會自己判斷）

這幾項是這次交接過程中浮現、但沒有明確答案的參數/門檻，目前程式碼裡都先填了一個暫定值並記錄選擇理由，**在拿到真正的決策之前先當作已知的不確定性看待，不要當成定案**：

1. **血壓計時鐘合理性檢查的容許誤差（`DEVICE_CLOCK_SANITY_WINDOW_MS`，目前 7 天，見第 8.5 節第 22 點）**：使用者已經反饋 7 天太長，但目前還沒有替代數字——這個值該怎麼訂，取決於實際部署情境下「裝置量測到 Pico 真正上傳」之間最壞情況可能拖多久（跟血壓計冷卻時間、WiFi 連線穩定度、裝置本身待機時長都有關），不是我能單方面決定的技術問題，需要跟主持人/臨床端確認合理的門檻後再調整程式碼。
2. **血氧計的官方休眠門檻未知（見第 3 節冷卻時間表格）**：額溫槍（1 分鐘）、血壓計（3 分鐘）都已經有使用者確認的官方數字，血氧計目前的 5 秒冷卻只是沿用舊值的暫定值，不是根據任何已知休眠規格設定的。需要確認血氧計實際的休眠設計（如果有的話），才能比照額溫槍/血壓計的做法重新設定冷卻時間、在「裝置真的睡著」跟「多久抓到新量測」之間取得平衡。
3. **已上傳紀錄保留筆數（`MAX_UPLOAD_HISTORY`，目前 200 筆，見第 5/8.5 節）**：主持人要求上傳後不要立刻清除本機資料，但沒有指定精確的保留筆數或天數，200 筆是根據粗略的使用量假設（一天約 20 筆、涵蓋 1~2 週）推算出來的，沒有實際使用數據驗證過。需要確認：(a) 這個數字是不是符合預期的保留範圍；(b) 保留依據要不要改成天數（例如「保留最近 30 天」）而不是固定筆數——目前選筆數是因為實作起來比時間範圍簡單（固定大小、不用額外處理時間換算跟過期判斷），如果臨床上更在意「保留多久」而不是「保留幾筆」，需要重新設計成以時間為準的版本。

## 8. 修復記錄與協定反推方法（給之後除錯/擴充新裝置參考，不是待辦）

按時間順序，只記錄根因跟修法，細節請直接看對應檔案的程式碼註解：

1. **【最關鍵】WiFi 連線「明明成功卻被誤判逾時砍斷重建」**：`cyw43_wifi_link_status()` 有時候不會準時回報 `CYW43_LINK_UP`，即使 lwIP 的 DHCP 早就拿到合法 IP。舊邏輯只信任這個狀態，等到逾時後把已經談成的連線整個砍掉重建，導致 WiFi 永遠連不上。**修法**：改成直接檢查 `netif_is_up()` 且 IP 不是 `0.0.0.0`（`mode_upload.c`）。
2. **AP_CONFIG 表單「填了 SSID 卻永遠收到空字串」**：HTTP headers 跟 body 常被 TCP 拆成不同封包送達，舊邏輯看到 `\r\n\r\n` 就急著解析表單，body 可能還沒送到。**修法**：解析前先比對 `Content-Length` 跟目前已收到的 body 長度（`mode_ap_config.c` 的 `parse_content_length()`）。
3. **`cyw43_wifi_scan()` 剛從藍牙模式切過來偶爾失敗（`-CYW43_EPERM`）**：STA 介面還沒真的啟用。**修法**：`cyw43_arch_enable_sta_mode()` 加重試迴圈確認介面真的起來才掃描。
4. **待傳紀錄「上傳失敗後永遠不會重試」**：`storage_pending_records()` 篩選條件寫錯，只挑 `PENDING`，`FAILED` 的紀錄永遠不會再被重傳。**修法**：篩選條件改成 `PENDING` 或 `FAILED`。
5. **額溫槍/血氧計共用同一份 GATT handle 快取**：導致連過額溫槍之後接著連血氧計用錯 handle 查詢失敗。**修法**：依裝置種類（`fora_device_kind_t`）分開快取（`mode_ble_receive.c` 的 `s_handle_cache[]`）。
6. **idle timeout 從「進入 BLE_RECEIVE 模式」就開始倒數，不是從「收到資料」開始**：血壓計這種要 30-45 秒才推播一次的裝置，量測進行中就被切去 UPLOAD、逼著斷線。**修法**：加一個「這一輪有沒有收到過任何資料」的旗標，收到第一筆之前完全不檢查逾時（`s_got_any_reading_this_session`）。
7. **待傳生理資料原本只存在 RAM，斷電就遺失**：**修法**：整份寫進 flash（`storage.c` 的 `persist_pending_records()`），開機時讀回。**注意：沒有 wear-leveling**，每次新增一筆或上傳結果都會整份覆寫一次，正式量產前需評估升級成 littlefs（見第 9 節）。
8. **裝置量測完持續廣播期間反覆被重新連線、喚醒**：改成跨模式切換都持續有效的冷卻時間（`DEVICE_RECONNECT_COOLDOWN_MS`，依裝置種類分開設定，見第 3 節）。
9. **同類型資料重複累積、上傳好幾筆重複/過時的值**：跟上一點同根因。**修法**：`storage_append_record()` 先檢查有沒有同類型還沒上傳成功的舊紀錄，有的話直接覆蓋，不再往陣列後面疊加。
10. **上傳的體溫數值小數位數異常**（例如 `36.79` 而非 `36.8`）：舊版 `format_value()` 用無條件捨去手算小數，浮點數誤差造成截斷。**修法**：先四捨五入到最接近的 0.1（體溫）或整數再格式化。
11. **上傳其實成功了，卻一直被裝置自己判斷成失敗**：HTTP 回應可能被 TLS record／pbuf 邊界切成好幾段，`HTTP/1.1 200 OK` 剛好被切開時舊邏輯找不到完整的 "200"。**修法**：把收到的內容累積進跨 callback 持續存在的 buffer 再比對（`upload_api.c` 的 `upload_ctx_t.header_buf`）。
12. **修完上面那個之後還是失敗，這次是真的 400 Bad Request**：`patient_id` 欄位混進了控制字元（成因不明，可能是很久以前測試殘留在 flash 裡的資料），沒跳脫直接塞進 JSON 導致伺服器 `json.loads()` 失敗。使用者自由輸入的欄位（`patient_id`、SSID、密碼）內容完全不受韌體控制，塞進任何有格式規則的地方（JSON、QR code、EPD 字型）之前都必須先跳脫/過濾。**修法**：新增 `append_json_escaped()`。
13. **UPLOAD/AP_CONFIG/錯誤畫面顯示完之後，回到 BLE_RECEIVE 卻可能永遠不刷新，面板卡在舊畫面**：這幾個畫面是直接畫、直接刷新，不會更新 BLE_RECEIVE 那邊「上次真的畫了什麼」的快照，內容剛好相同時會被誤判成「沒有變化」而不刷新。**修法**：加 `s_ble_screen_is_current` 旗標，其他畫面顯示時清成 false，強制下一次 BLE_RECEIVE 至少刷新一次。
14. **`mode_ble_receive.c` 的 debug log 也有一份跟第 10 點一樣的浮點數截斷 bug**：只影響 log 顯示精度，數值本身沒錯。**修法**：比照第 10 點四捨五入。

**血壓計協定反推方法（給以後擴充新裝置參考）**：真正協定是靠反編譯官方 Windows 程式找到的，不是靠猜或靠 nRF Connect 觀察。官方電腦端程式 `FORA Health Care Management System_BLE`（安裝在 `%APPDATA%\FORA Health Care Management System_BLE\`）裡的 `BLE_PCLink_Library.dll`（`TaiDoc.BLE_PcLink` 命名空間），用 ILSpy 反編譯後直接看到組指令/解析回應的原始碼。反編譯工具鏈：`dotnet tool install --global ilspycmd --version 8.2.0.7535`（**必須釘住版本**，`--version latest` 會因套件缺 `DotnetToolSettings.xml` 安裝失敗）+ `Microsoft.DotNet.Runtime.6`（`ilspycmd` 執行期依賴，跟系統其他 .NET 版本無關）。這個方法的前提是廠商有提供電腦端程式、且沒有額外加殼/混淆，**不是每次遇到協定不明的裝置都能複製的路徑**。血糖協定（第 6.4 節）也是用同一支 DLL、同一套工具鏈反推出來的，找的是 `BloodGlucose2in1`/`GenBgmAndBpmMeter` 這幾個 class。

**血壓計上線後、實機重複測試發現並修好的問題**：

15. **同一筆血壓記錄被重複當成新資料上傳**：見第 6.3 節「判重」的說明，改用裝置量測時間戳（`fora_protocol_decode_measured_key()`）判重。
16. **判重生效後，idle timeout 邏輯還是會切去 UPLOAD 白跑一趟 WiFi**：判重會讓收到讀值但不放進待傳佇列，原本的邏輯不管佇列裡有沒有東西都會切換。**修法**：切換前先檢查 `storage_pending_count() > 0`（見第 3 節狀態機流程圖）。
17. **血壓計冷卻時間從 60 秒拉長到 4 分鐘**：見第 3 節冷卻時間表格。**已實機確認**：2026-08-05 晚間約 19:30 燒錄的版本，裝置在冷卻期間真的會自動休眠，見第 7.2 節第 3 點。
18. **血壓計顯示/上傳的時間戳改用裝置自己的量測時間**：不再用「Pico 收到 BLE 通知的時間」。實機驗證過：(a) 解碼出來的時分跟量測當下的實際本地時間吻合；(b) NTP 校時失敗時，血壓的時間戳依然正確（因為完全不依賴 wall_clock）。**當時的已知風險（2026-08-06 已修好，見第 8.5 節）**：完全信任裝置回報的時間戳，沒有做合理性檢查——現在會跟 Pico 的 NTP 校時結果比對，差距太大就退回用 Pico 收到時間，還沒有實機驗證，見第 7.2 節第 12 點。
19. **`VITAL_TYPE_PULSE_RATE` 被血氧計跟血壓計共用，導致跨裝置判重誤判**：兩種裝置的脈搏數值只要剛好相同/不同就會互相干擾對方的判重結果（實測抓到過一次：血壓計的脈搏覆蓋了血氧計的「最後讀值」，導致血氧計下一筆真正的重複讀值被誤判成新資料）。**修法**：`vital_record_t` 新增 `source_kind` 欄位，判重時額外要求來源裝置也要相同（見第 5 節）。
20. **電子紙「Scanning」心跳刷新間隔改成 180 秒**：比照 Waveshare 資料手冊建議的刷新間隔下限（見第 12.5.1 節），這是唯一一個「定期、沒有實際新事件也會觸發」的刷新來源，跟「180 秒建議值不嚴格遵守」的決定不衝突（那個決定針對的是有新事件時不要延遲，心跳沒有這個顧慮）。

## 8.5 2026-08-06 裝置不在身邊、無法實機操作期間完成的軟體工作（全部尚未燒錄/實機測試）

這批全部是純程式碼工作，寫完只跑過 `cmake --build` 編譯確認沒有語法錯誤，**沒有任何一項燒錄過或實機測試過**，見第 7.2 節第 11-14 點的對應驗證項目：

21. **血壓計解析加上記錄類型旗標檢查＋接上血糖協定**：見第 6.3 節「已知正確性缺口」跟第 6.4 節。`fora_protocol_parse_reading()` 原本無條件把血壓計 kind 的回應當成血壓資料解析，現在會先看 `byte[2]` bit7：0 就照第 6.4 節的格式解析血糖（回傳 `VITAL_TYPE_GLUCOSE`，官方程式對 65535/255 這兩個特殊值視為無效讀值也一併照做），非 0 才照原本的血壓邏輯解析。`common.h` 的 `VITAL_TYPE_GLUCOSE` 註解、`display_status.c` 的血糖列註解也一併更新（原本說「協定還沒確認/還沒接」，現在協定已經接上，只是沒有實機驗證過）。
22. **血壓計時鐘合理性檢查**：見第 6.3/7.1 節。`mode_upload.c` 新增 `DEVICE_CLOCK_SANITY_WINDOW_MS`（7 天），上傳前把裝置量測時間戳換算成 epoch ms 之後，如果 Pico 自己已經 NTP 校時過，會跟現在時間比對，差距超過這個範圍就判定裝置時鐘不可信、退回用 Pico 收到 BLE 通知的時間換算；如果 Pico 自己都還沒校時過，沒有基準可以比對，照樣採用裝置時間戳（唯一可用的真實時間來源）。
23. **上傳伺服器網址、認證金鑰改成可透過 AP_CONFIG 表單設定**：`common.h` 的 `device_config_t` 新增 `upload_server_host`／`upload_api_key` 兩個欄位（留空的行為跟 WiFi 密碼欄位一致：留空 = 不變更目前值，不是清空）。`upload_api.c` 的 `upload_api_post_batch()` 簽名改成接收這兩個值，留空時退回內建的測試預設主機（`UPLOAD_SERVER_HOST_DEFAULT`）、不加認證標頭；有填認證金鑰的話會加一個 `X-API-Key` 標頭（先過濾非可印出 ASCII 字元，避免使用者輸入裡的怪字元弄亂 HTTP 標頭格式）。**伺服器端目前沒有任何後端會真的檢查這個標頭**（見第 7.1 節第 2 點），這只是把 Pico 端「能送出認證資訊」這件事準備好。
24. **AP 熱點密碼、SSID 都改成每台裝置唯一衍生**：不再寫死 `"gateway123"`/`"PicoGateway-Setup"`。`mode_ap_config.c` 新增 `generate_ap_password()`，從 RP2040 flash 晶片出廠燒錄的 64-bit 全球唯一序號（`pico_get_unique_board_id()`，`CMakeLists.txt` 新增連結 `pico_unique_id`）衍生出 8 位十六進位字元的密碼；`generate_ap_ssid()` 從 CYW43 晶片的出廠 MAC 位址（`cyw43_wifi_get_mac()`）取後 2 bytes 衍生出 `PicoGateway-Setup-XXXX` 這種帶 4 位十六進位字尾的 SSID——**這是使用者在裝置不在身邊的這次交接裡追加提出的需求**：多台裝置部署在同一場所時，如果 SSID 都叫一模一樣的名稱，手機的 WiFi 列表會出現好幾個同名網路，分不出要連哪一台。SSID、密碼同一台裝置每次進熱點模式都相同，且兩者都會顯示在電子紙的 AP_CONFIG 畫面上（不需要額外印貼紙，螢幕本身就是資訊來源）。
25. **TLS 憑證驗證框架**：新增 `src/upload_tls_ca_cert.h`，`UPLOAD_CA_CERT_PEM` 目前是空字串（沒有正式後端網址可以嵌入真正的憑證）。`upload_api.c` 呼叫 `altcp_tls_create_config_client()` 時改成依這個陣列是否有內容決定要不要帶入 CA——lwIP 的封裝收到非空 CA 內容會自動把驗證模式改成要求驗證，沒有的話退回不驗證並在 log 印出明顯警告（不再是靜默的不安全狀態）。等正式後端確定之後，把憑證 PEM 貼進這個檔案就完成，不用改其他程式碼，檔案裡有詳細的取得憑證方式說明。
26. **電子紙 Phase 3：局部刷新排程——已實作又移除**：一開始在 `display_status.c` 的 `end_frame_and_refresh()` 加了 `prefer_partial` 參數（BLE_RECEIVE 優先用局部刷新，其餘畫面維持全刷，加了 `PARTIAL_REFRESH_MAX_CONSECUTIVE` 定期強制全刷清殘影）。**後來重新檢視這個決定時發現：沒有任何一次實測真的觀察到「需要局部刷新」的具體問題**——原本設想的理由（全刷卡住 2-3 秒可能讓 BLE 主迴圈錯過血壓計/血氧計的短暫廣播窗口）從頭到尾只是理論推測，BLE_RECEIVE 本來就靠內容比對、刷新頻率不高，不是真的常常在跟短暫廣播窗口搶時間。局部刷新換來的好處（0.6 秒 vs 3 秒、不閃黑）要用「完全沒有實機驗證過的呼叫順序」跟「疊代殘影需要額外清理邏輯」這些真實風險去換，划不來，所以拿掉了，只保留全刷。見第 12.5 節。
27. **已上傳紀錄保留機制（主持人明確提出的需求）**：見第 5 節「已上傳紀錄保留機制」的完整說明。`storage.c` 新增獨立的環狀緩衝（`s_upload_history[]`），上傳成功的紀錄除了從待傳佇列移除，也會另外複製一份進這個緩衝，保留最近 200 筆、持久化在 flash。**200 筆是概略估計，不是精確計算或使用者指定的數字**，選這個數字的假設（單一個案一天約 20 筆讀值、涵蓋 1~2 週）沒有實際使用數據驗證過，見第 7.3 節待確認事項。目前沒有任何介面可以讀出這份歷史，純粹是先把資料留著。

## 9. 已知限制 / 正式上線前必須處理

1. **TLS 憑證驗證框架已接好，但還沒有真正的憑證**（見第 8.5 節第 25 點、`upload_tls_ca_cert.h`）：`UPLOAD_CA_CERT_PEM` 目前是空字串，等於還是不驗證，只是現在會在 log 印出明顯警告、且日後補上憑證不用改程式碼。正式上線前必須把目標伺服器的憑證/CA PEM 貼進這個檔案。
2. **上傳伺服器網址、認證金鑰已經可以透過 AP_CONFIG 表單設定**（見第 8.5 節第 23 點），但**伺服器端還沒有任何後端會真的驗證這個認證金鑰**——正式後端需要自己實作檢查 `X-API-Key` 標頭的邏輯，不然這個機制形同虛設。
3. **熱點設定模式的密碼已經改成每台裝置唯一衍生**（見第 8.5 節第 24 點），這條限制已解除。
4. **待傳資料的 flash 持久化沒有 wear-leveling**，見第 8 節第 7 點，寫入頻率高的話會較快耗損，正式量產前需評估升級成 littlefs（這是比較大的改動，這次沒有做）。

## 10. 專案目錄結構

```
pico-vitals-gateway/
├── PROJECT_PLAN.md          # 這份文件：狀態總覽、協定細節、測試交接事項
├── FIRMWARE_FILES.md        # 逐檔案說明（更細節的程式碼導覽）
├── README.md
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── lwipopts.h
├── btstack_config.h
├── mbedtls_config_override.h
├── dhcpserver.c/.h          # 從 pico-examples 複製，AP_CONFIG 熱點用
├── dnsserver.c/.h           # 同上
├── epd/                     # 從 Waveshare Pico_ePaper_Code 複製的 2.9" 電子紙驅動，見第 12 節
│   ├── DEV_Config.c/.h      # SPI1/GPIO 底層存取，改過：拿掉重複的 stdio_init_all()
│   ├── EPD_2in9_V2.c/.h     # SSD1680 控制器指令層（全刷/局部刷新/睡眠）
│   ├── GUI_Paint.c/.h       # 畫面 framebuffer + 繪圖/文字 API
│   ├── Debug.h
│   └── Fonts/               # 只保留 ASCII 字型（font8/12/16/20/24），沒帶簡體中文字型
├── qrcode/                  # Nayuki QR-Code-generator，AP_CONFIG 的 WiFi QR code 用，見第 12.7 節
│   ├── qrcodegen.c
│   └── qrcodegen.h
├── test_server/             # 本機測試用的簡易上傳伺服器（Python）
└── src/
    ├── main.c
    ├── common.h             # 共用資料型別（device_config_t / vital_record_t / vital_type_t），見第 5 節
    ├── state_machine.c/.h
    ├── mode_boot_select.c/.h
    ├── mode_ap_config.c/.h
    ├── mode_ble_receive.c/.h
    ├── fora_protocol.c/.h   # 三種 FORA 裝置的協定解析，見第 6 節
    ├── mode_upload.c/.h
    ├── upload_api.c/.h
    ├── wall_clock.c/.h      # NTP 校時，boot-relative ms 換算成真實世界 epoch ms
    ├── led_status.c/.h
    ├── display_status.c/.h # 電子紙顯示器封裝，見第 12 節
    └── storage.c/.h         # 設定值 + 待傳紀錄的 flash 持久化
```

## 11. 里程碑

1. ✅ 環境就緒。
2. ✅ 骨架就緒（狀態機 + LED + BOOTSEL 開機視窗）。
3. ✅ 熱點設定模式：已實機驗證手機連線、captive portal 自動彈出、表單送出、設定值讀回並帶入既有值。
4. ✅ BLE 接收模式：額溫槍、血氧計、血壓計三種裝置都已完整實機驗證（血壓計走反編譯確認的自訂協定，見第 6.3/8 節）。🔧 血糖協定已反推確認並實作（見第 6.4/8.5 節），**還沒實機驗證**。
5. ✅ 上傳模式：WiFi 連線 + HTTPS 上傳已完整驗證多次成功，含多種認證模式重試、DHCP 誤判 bug 修復、NTP 校時、失敗自動重試＋斷電持久化。🔧 伺服器網址/認證金鑰可設定、TLS 憑證驗證框架已接好（見第 8.5 節），還沒有正式後端網址可以完成最後一步。
6. ⬜ 24 小時連續運作測試，觀察 flash 抹寫、記憶體、藍芽/WiFi 穩定度——**交接給驗證工程師執行**。
7. ✅ 電子紙顯示器（Waveshare Pico-ePaper-2.9）Phase 1+2 都已燒錄實機驗證：接線/文字顯示正常，四種模式真實資料畫面都測過，詳見第 12 節。🔧 Phase 4 提前做的 AP_CONFIG WiFi QR code（12.7 節）程式碼已寫完、編譯過關，**還沒實機掃碼測試**。~~Phase 3（局部刷新排程）~~已評估後決定不做，見第 12.5 節——沒有實測觀察到的具體場景真的需要它。

## 12. 電子紙顯示器（Waveshare Pico-ePaper-2.9）規劃

> 目前裝置只靠 LED 燈號（見第 4 節）當作使用者能看到的唯一回饋，燈號規則需要背起來才看得懂，個管師/家屬完全看不出裝置在幹嘛。接上這片 296×128 電子紙後，目標是讓螢幕變成「不用懂技術也看得懂」的儀表板，LED 保留當底層、隨時看得到的心跳/錯誤指示（螢幕更新慢，LED 補足即時性），兩者不是取代關係。

### 12.1 硬體

- 型號：Waveshare Pico-ePaper-2.9（SSD1680 控制器，V2 時序），296×128，黑白（4 灰階但目前只用黑白）。
- 直接疊在 Pico W 上當 HAT，走硬體 **SPI1**：DIN→GP11、CLK→GP10、CS→GP9、DC→GP8、RST→GP12、BUSY→GP13、VCC→VSYS、GND→GND。
- 跟 CYW43（WiFi/藍牙晶片）走的是完全不同的內部接腳，不衝突。

### 12.2 驅動來源

- 從 Waveshare 官方 `Pico_ePaper_Code` repo（`c/lib/`）複製過來，放在專案根目錄 `epd/`（跟 `dhcpserver.c`／`dnsserver.c` 一樣是「從外部複製進來的第三方程式碼」，不算我們自己維護的邏輯）：
  - `DEV_Config.c/.h`：SPI1 + GPIO 底層存取。**改過一處**：原版 `DEV_Module_Init()` 內部會呼叫 `stdio_init_all()`，但 `main.c` 已經呼叫過一次，拿掉重複呼叫。
  - `EPD_2in9_V2.c/.h`：SSD1680 指令層，提供 `EPD_2IN9_V2_Init/Clear/Display/Display_Base/Display_Partial/Sleep`。
  - `GUI_Paint.c/.h`：framebuffer + 畫點/線/框/文字 API（`Paint_DrawString_EN`、`Paint_DrawNum` 等）。
  - `Fonts/`：只保留 ASCII 字型（`font8/12/16/20/24`），**沒有帶**官方 demo 附的簡體中文字型（`font12CN`/`font24CN`，GB2312 編碼，而且只內建 demo 用到的幾個字，帳面上對這個專案沒用）。
- 授權：Waveshare 官方範例程式碼，MIT 風格授權（檔頭有附）。

### 12.3 中文顯示的限制（重要，會卡住 Phase 2）

- 電子紙的字型是「點陣圖直接燒進 flash 的常數陣列」，不是即時算圖，官方函式庫只內建 demo 用到的少數簡體字。**個案姓名／個管師資訊是使用者在 AP_CONFIG 表單現場輸入的任意繁體中文字串，沒辦法事先烘焙進字型**，這點跟英數字/數字（體溫、血氧、脈搏、血壓數值）完全不同——數字用內建 ASCII 字型就能顯示。
- **決定：採用方案 A**——螢幕上只顯示個案編號（`patient_id`）取代姓名，姓名全名留在上傳的資料裡，不上螢幕。
  - **注意**：`patient_id` 一樣是沒有限制輸入內容的自由文字欄位，使用者一樣可能填中文進去。`GUI_Paint.c` 的 `Paint_DrawChar()` 是用 `(char - ' ')` 直接算 flash 位址偏移量，收到 UTF-8 多位元組中文字（每個 byte 被當成獨立字元）算出來的偏移量可能超出字型表範圍，輕則亂碼、重則讀到無效 flash 位址讓裝置當機。**畫面顯示前一律先過濾成只保留可印出的 ASCII 字元（0x20–0x7E）**（`sanitize_ascii()`）。
  - 曾考慮過的替代方案（已否決）：方案 B 拉一份完整繁體中文點陣字型（檔案通常上百 KB 到數 MB，RP2040 只有 2MB flash 且已被 WiFi/BTstack/mbedTLS 佔掉不少，風險較高）；方案 C 另外收集羅馬拼音/暱稱欄位（多一道使用者輸入步驟）。

### 12.4 軟體模組設計

`src/display_status.c/.h`，跟 `led_status` 平行、風格一致：

```c
void display_status_init(void);
void display_status_show_boot_test(void);   // 開發驗證接線用，main.c 目前不會自動呼叫

void display_status_show_ap_config(const char *ap_ssid, const char *ap_password,
                                    const device_config_t *existing_config);
void display_status_show_upload(const char *ssid, const char *result_text);
void display_status_show_error(const char *message);

void display_status_set_ble_receive(const device_config_t *config, const char *status_text);
void display_status_poll(void);   // 非阻塞，只給 BLE_RECEIVE 的主迴圈呼叫

void display_status_format_clock(uint64_t boot_ms, char *out, size_t out_size);
```

AP_CONFIG／UPLOAD／錯誤畫面的內容在該模式執行期間變動不頻繁，`mode_ap_config.c`／`mode_upload.c` 在幾個關鍵時間點直接呼叫對應的 `show_xxx()`（直接畫、直接刷新），不需要輪詢。BLE_RECEIVE 不一樣：它的主迴圈是持續數十秒到數分鐘的緊迴圈，內容會持續變動，所以拆成 `set_ble_receive()`（只更新「想顯示的內容」，便宜）+ `poll()`（`mode_ble_receive.c` 主迴圈每輪呼叫，內部比對「這次的內容」跟「上次真的畫到螢幕上的內容」，只有真的不一樣才觸發一次全刷）——比對包含數值/時間戳（含 `device_measured_key`）/筆數/狀態文字，避免時間流逝本身觸發刷新風暴（詳見 12.5 節）。

### 12.5 全刷／局部刷新與非阻塞的取捨

- 全刷（`EPD_2IN9_V2_Display`/`Display_Base`）約 3 秒、會整片閃黑再回來，但沒有殘影；局部刷新（`EPD_2IN9_V2_Display_Partial`）約 0.6 秒不會閃，但疊代次數多了畫面會有殘影，需要定期強制全刷清掉。**2026-08-06 曾經接上局部刷新又拿掉了**（見第 8.5 節第 26 點）：重新檢視後發現沒有任何實測觀察到的具體場景真的需要它——原本設想的理由（全刷卡住 2-3 秒可能讓 BLE 主迴圈錯過裝置的短暫廣播窗口）只是理論推測，從沒真的觀察到發生過，局部刷新換來的速度/不閃黑好處，要用沒有實機驗證過的呼叫順序風險跟疊代殘影的額外清理邏輯去換，不值得。**目前維持只用全刷**。
- **`EPD_2IN9_V2_ReadBusy()` 是阻塞式忙等**（`while (BUSY==1) sleep_ms(50)`），全刷一次會讓主迴圈卡住 2-3 秒，會影響 BLE GATT 事件處理／BTstack run loop 的即時性。解法是 `display_status_poll()` 內部做「內容真的變了才刷新」的比對，而且比對刻意不含會隨時間漂移的「X 分鐘前」文字——這是刻意的取捨：全刷要 3 秒，如果為了讓文字即時而定時刷新，BLE 迴圈的即時性會被拖累；e-paper 本來就是「一眼瞄過去看大概」的裝置，不需要秒級精確。
- 生理讀值本身天生就不會太頻繁（同一種裝置有冷卻時間、不同裝置量測要數秒到數十秒），加上一次量測（例如血壓計同時回傳收縮壓+舒張壓+脈搏）會在同一個事件循環內連續呼叫多次 `storage_append_record()`、`poll()` 下一輪才執行一次，所以不需要額外加節流計時器，內容比對本身就已經天然稀疏。
- **尚未驗證**：確認 BLE_RECEIVE 連續收到好幾筆讀值時畫面是不是每次都有即時更新，或修改 `MAX_REFRESH_INTERVAL_MS` 成短時間驗證 24 小時強制刷新邏輯有沒有真的觸發（見第 7.2 節第 10 點）。

#### 12.5.1 面板保護：睡眠與刷新頻率限制（硬體規格書要求）

Waveshare 資料手冊列了幾點面板保護要求，**其中「不能長時間通電/必須睡眠」是硬性規定（違反會造成不可逆的實體損壞）**，其他幾點是建議值：

- 電子紙面板**不能長時間維持通電/高電壓狀態**（硬性）：不刷新的時候必須進睡眠模式（或斷電），不然膜片會壞掉、修不好。
- 刷新間隔建議至少 180 秒（建議值）。
- 建議至少每 24 小時要刷新一次，就算內容沒變也要刷（建議值，避免長時間靜態顯示造成殘影/老化）。
- 長期不使用的話，面板要先刷成全白再收起來存放（實體庫存/備品管理層面的事，跟韌體邏輯無關，這裡只是記錄需求，沒有做對應的韌體功能）。

**決定：180 秒建議值不嚴格遵守，以使用方便性為原則**——BLE_RECEIVE 收到新讀值、UPLOAD 顯示結果都是使用者/個管師想立刻看到的狀態，硬性延遲 3 分鐘才顯示，體驗上得不償失。「不能長時間通電」這條硬性規定則不受影響，照樣嚴格遵守。**唯一的例外是「Scanning」心跳更新**（見 12.6 節）：這是唯一一個「定期、沒有實際新事件也會觸發」的刷新來源，沒有「越快越好」的理由，改成比照 180 秒建議值。目前 `display_status.c` 的實際作法（`end_frame_and_refresh()` 是唯一把關的地方，四種畫面都經過它）：

1. **每次刷新都是「喚醒 → 畫 → 睡眠」一整套，沒有例外**：`EPD_2IN9_V2_Init()`（內部會先做硬體 Reset，同時也是喚醒深度睡眠的標準程序）→ `EPD_2IN9_V2_Display_Base()` → `EPD_2IN9_V2_Sleep()`。面板在兩次刷新之間永遠是睡眠狀態，不會停在「醒著但沒在畫」的高電壓狀態。
2. **沒有 180 秒下限**：呼叫端要求刷新就會真的刷新，不會被延遲（心跳更新例外，見上面）。
3. **保留 24 小時強制刷新**：`display_status_poll()` 裡，就算內容完全沒變，距離上次刷新超過 24 小時也會觸發一次刷新（AP_CONFIG／UPLOAD 執行時間本來就短，用不到這個機制，只有 BLE_RECEIVE 這個 24/7 常駐模式需要）。
4. `end_frame_and_refresh()` 回傳值目前恆為 `true`（沒有任何情況會跳過），但介面還是設計成回傳 bool，之後如果又要加別的跳過條件（例如偵測到面板故障）不用改呼叫端。
5. `main.c` 開機時不會自動呼叫 `display_status_show_boot_test()`：這個畫面對個管師/家屬沒有意義，只是開發驗證接線用的，需要時手動呼叫確認硬體即可。
6. `display_status_init()` 只做 SPI/GPIO 跟 framebuffer 初始化，**不會**呼叫 `EPD_2IN9_V2_Init()/Clear()`（那本身就是一次刷新）——面板的硬體初始化延後到第一次真的要刷新畫面時，由 `end_frame_and_refresh()` 觸發，避免開機時做兩次背靠背的刷新。

### 12.6 畫面內容（Phase 2 已實作並實機驗證）

| 模式 | 畫面內容 | 對應函式 |
|---|---|---|
| AP_CONFIG 熱點設定 | 熱點 SSID/密碼文字、目前已存的個案編號（ASCII 過濾過，沒設定過就顯示 `(unset)`）、操作提示、WiFi QR code（見 12.7 節） | `display_status_show_ap_config()` |
| BLE_RECEIVE | 個案編號（方案 A）、目前狀態（`Scanning (MM/DD HH:MM)`，見下方心跳說明）、體溫/血氧/脈搏/**血糖（協定已實作，見 6.4 節，沒量過的話顯示 `-- (never)`）**/血壓各自最後一筆數值＋時間戳（已校時顯示 `MM/DD HH:MM` 絕對時間，血壓計顯示的是**裝置自己的量測時間**而非 Pico 收到時間，見第 6.3 節；未校時且無裝置時間戳顯示 `unsynced,+Nm`）、**最後一次成功上傳時間**、待上傳筆數 | `display_status_set_ble_receive()` + `display_status_poll()` |
| UPLOAD | WiFi SSID + 目前階段/結果文字（`Connecting...`/`Success (N records)`/`Failed, will retry`） | `display_status_show_upload()` |
| 錯誤 | 一句英文錯誤訊息，取代難記的三連閃燈號 | `display_status_show_error()` |

**「Scanning」狀態文字的心跳設計**：故意不用靜態的 `"Scanning..."`——電子紙斷電/當機時畫面會凍結在最後一次刷新的內容，如果狀態文字本身不含時間資訊，使用者沒辦法從畫面分辨「裝置還活著、只是沒掃到裝置」跟「裝置已經當機」。`update_scanning_status()` 組出帶時間戳的狀態文字，並在 `mode_ble_receive_run()` 主迴圈裡每 180 秒（比照 Waveshare 資料手冊建議的刷新間隔下限，見 12.5.1 節）定期重新呼叫一次，讓時間戳在沒有裝置連線活動時依然會前進。**尚未實機視覺驗證**：程式邏輯已審視過、序列埠 log 能間接佐證運作時機，但還沒有真的空出一段裝置都不在旁邊廣播、超過 180 秒的安靜時間，肉眼確認面板上的時間戳有沒有正確前進（見第 7.2 節第 9 點）。

局部刷新排程（減少全刷閃爍/縮短刷新時間）曾經做過又移除了，見上面 12.5 節——沒有實測觀察到的具體場景真的需要它，決定維持只用全刷。

### 12.7 AP_CONFIG 的 WiFi QR code（原規劃 Phase 4 提前）

- 手機相機掃到 `WIFI:T:WPA;S:<ssid>;P:<password>;;` 這個特定前綴的字串會自動跳出系統內建的「加入 WiFi」提示——這是業界慣例（源自 ZXing），**QR code 本身沒有什麼特殊格式/模式**，跟顯示純文字/網址的 QR code 用的是同一套編碼方式，差別只在字串內容。SSID/密碼裡如果出現 `\`、`;`、`,`、`:`、`"` 這幾個字元，依慣例要加反斜線跳脫（`display_status.c` 的 `append_escaped_wifi_field()`）——`AP_SSID` 是寫死字串、`generate_ap_password()` 衍生出的密碼固定是 `pico-` 加 8 位十六進位字元，兩者都不會出現這些字元，但當初先做完整這件事現在證實是對的：密碼後來（見第 8.5 節第 24 點）真的從寫死字串改成動態產生了，這個跳脫邏輯不用跟著改。
- 這個 QR code 編碼的是**熱點本身**的 SSID/密碼（`generate_ap_ssid()`/`generate_ap_password()` 衍生出的每台裝置專屬 SSID/密碼，見第 8.5 節第 24 點，讓手機能連上 Pico 的設定用熱點），**不是**個案要接的目標 WiFi（`device_config_t.wifi_ssid`/`wifi_password`，那組帳密是使用者在熱點頁面的表單裡填的，不會出現在任何 QR code 上）。
- QR code 編碼器：從 Nayuki 的 `QR-Code-generator`（`c/qrcodegen.c`/`.h`，MIT 授權）複製進專案根目錄 `qrcode/`，純 C89、沒有外部依賴（只用標準函式庫），沒有改動任何一行。呼叫 `qrcodegen_encodeText()`，ECC 等級用 MEDIUM（可以容忍約 15% 資料損毀，兼顧掃描容錯率與 QR 大小），版本上限給 10（`qrcodegen_BUFFER_LEN_FOR_VERSION(10)` 對應的 buffer 只有 408 bytes，對這種 40~50 bytes 的短字串綽綽有餘，實際會落在 version 2~3 左右，遠用不到上限）。
- 畫在 AP_CONFIG 畫面右側（`draw_qr_code()`，把 QR 的每個 module 依比例放大成好幾個實際像素畫上去，四周留白靠 `Paint_Clear(WHITE)` 清出來的背景自然滿足，不用額外處理），文字（SSID/密碼/個案編號/操作提示）留在左側，兩者之間留了足夠間距。
- **尚未實機掃碼驗證**，見第 7.2 節第 8 點。
