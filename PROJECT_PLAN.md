# Raspberry Pi Pico W 生理訊號中繼裝置 — 專案計畫

> 這份文件是目前最新的狀態總覽與交接文件（2026-08-04 整理）。舊版內容（2026-07-31）已整合進來或標記為過時；如果內容跟程式碼有衝突，一律以程式碼為準，並回頭更新這份文件。

## 目前進度摘要（2026-08-04）

- ✅ 環境、骨架（狀態機、BOOTSEL 開機視窗、LED 燈號、flash 儲存）已完成並實機驗證。
- ✅ **熱點設定模式（AP_CONFIG）已實機驗證**：手機連上熱點會自動跳出設定頁（captive portal），可掃描附近 WiFi、選擇或手動輸入 SSID、填個案資訊，設定頁會帶入目前已存的值，密碼欄位留空 = 不變更。
- ✅ **BLE 接收模式支援三種 FORA 裝置**，額溫槍、血氧計已完整驗證能正確量測上傳；血壓計已能連線/訂閱，但**還沒真正收到過一次完整量測資料**（見下方「需要測試」）。
- ✅ **WiFi 連線 + 上傳模式已完整打通並多次驗證成功**：連上真實家用路由器（WPA2）、透過 Cloudflare Tunnel 打 HTTPS 上傳到測試網站，資料正確顯示。過程中修掉一個很關鍵的 bug（DHCP 明明成功卻被誤判逾時），詳見下方「本次修好的重要 bug」第 1 點。
- ✅ 待傳資料現在會持久化到 flash（斷電/上傳失敗不會遺失）。
- ✅ 裝置量測時間會嘗試透過 NTP 換算成真實世界時間（校時失敗會 fallback 顯示開機經過時間，不會出錯，但正確性還沒充分驗證）。
- ⬜ 血壓量測的完整流程（充放氣、收到 indicate、解析出正確數字）尚未實測。
- ⬜ 血糖量測完全還沒開始（FORA D40 是血壓血糖二合一裝置，這次只做了血壓）。
- ⬜ 24 小時等級的耐用性測試完全沒做過——**這是這次交接給實習生的主要任務**。

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

1. 接收藍芽（BLE）生理量測裝置（目前：FORA IR42 額溫槍、FORA O2 血氧計、FORA D40 血壓計）傳來的數值。
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
  收到任一裝置的資料 → 寫入 storage（同時落地 flash）
  收到第一筆資料之後，開始倒數：距離「最後一筆資料」超過設定的秒數沒有新資料
    → [UPLOAD]
  （注意：在收到任何資料之前不會倒數/逾時，這樣血壓計那種要 30~45 秒才會
   推播一次的裝置，量測進行中不會被提早打斷去切模式）

[UPLOAD]      上傳模式，LED 快閃
  連上 WiFi → 嘗試 NTP 校時 → 上傳所有待傳紀錄
  無論成功或失敗 → 一律回到 [BLE_RECEIVE]（失敗的紀錄留著，下次會重試）

任何模式下偵測到嚴重錯誤 → LED 三連閃+停頓，記錄錯誤狀態
```

切換到任何 WiFi 相關模式前必須先關閉藍芽，反之亦然，此「單一無線擁有者」規則由 `state_machine.c` 統一把關。

`BLE_IDLE_UPLOAD_TRIGGER_MS`（`state_machine.c`）目前設定為 **5 秒**：收到任一裝置的第一筆資料後，5 秒內沒有新資料就切到 UPLOAD。因為現在是「收到資料才開始倒數」（見上面流程圖的說明），血壓計那種要 30-45 秒才推播一次的裝置不會被這個 5 秒卡住——它會一直停在等待狀態，直到真的收到資料，才開始倒數 5 秒。

裝置重新連線冷卻時間 `DEVICE_RECONNECT_COOLDOWN_MS`（`mode_ble_receive.c`）目前設定為 **60 秒**：某種裝置拿到讀值之後，60 秒內不會再重新連線同一種裝置，就算裝置還在廣播也不理它，讓它有機會真的休眠/閒置。這個冷卻時間**跨越 BLE_RECEIVE/UPLOAD 模式切換持續有效**（不是進 BLE_RECEIVE 模式就重置），這是修過的一個問題——原本每次重新進入 BLE_RECEIVE 都會重置「這輪讀過了」的記錄，如果裝置量測完還在持續廣播（常見行為），上傳一結束回到 BLE_RECEIVE 就會立刻被重新連線，導致裝置反覆被喚醒、還會產生一堆幾乎一樣的重複紀錄。60 秒是先抓的一個合理值，如果實測發現裝置醒著的時間比這個久（一直被重複連線）或太久沒辦法接受下一次真正的量測，這個數字需要調整。

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

- `device_config_t`：WiFi SSID / 密碼、個案姓名、個案編號、個管師資訊。
- `vital_record_t`：接收時間（`received_at_ms`）、量測類型（`vital_type_t`）、數值、上傳時間、上傳狀態。
- `vital_type_t`：`UNKNOWN`(0) / `TEMPERATURE`(1) / `SPO2`(2) / `PULSE_RATE`(3) / `SYSTOLIC`(4) / `DIASTOLIC`(5)。

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
- **跟額溫槍用完全相同的 Service/Characteristic UUID**（同一套 Nordic LED/Button Service pipe），但兩種裝置各自的 GATT attribute table 排列不同（血氧計前面多了好幾個標準服務），**handle 編號不通用**——這是本次修掉的一個 bug，`mode_ble_receive.c` 依裝置種類分開快取 handle。
- 訂閱、觸發指令跟額溫槍完全一樣（同一個 `FORA_TRIGGER_COMMAND`）。
- 回應格式（2026-08-03 實測、比對螢幕顯示數字回推確認）：`byte[0]==0x51`；
  - SpO2 = `(byte[3]<<8 | byte[2]) & 0x0FFF`，單位 %，**不用除以 10**（跟溫度的公式不同！）。
  - 脈搏 = `byte[5]`，單位 bpm，直接就是整數。
  - `byte[4]`／`byte[6]`／`byte[7]` 用途未知，目前忽略。
  - 一次通知會同時回傳這兩筆數值。實測範例：`51 26 61 00 3c 4c a5 05` → SpO2=97%、脈搏≈76（跟螢幕顯示的 71-80 bpm 範圍吻合）。

### 6.3 FORA D40 血壓計（血壓部分）（`FORA_DEVICE_BLOOD_PRESSURE`，kind=3）

- 判斷依據：名稱含 `"D40"`（實測裝置名稱是 `"FORA D40"`）。
- **跟前兩種裝置不同**：這台用的是**標準 Bluetooth SIG Blood Pressure Service**，不是自訂 pipe：
  - Service：`0x1810`（Blood Pressure）
  - Characteristic：`0x2A35`（Blood Pressure Measurement，官方規格屬性是 **Indicate**）
  - 裝置同時也宣告了跟前兩種裝置一樣的 Nordic LED/Button Service（`00001523.../00001524...`），推測是給血糖量測用，**目前完全沒探索**。
- **實際做法：連線後直接 Read 讀取 `0x2A35` 的值，不是訂閱 Indicate 等推播**（這是繞了一大圈才確認的，見下方說明）。裝置隨時把最後一次量測結果放在這個特徵值裡，連線後隨時 Read 都拿得到，不需要在量測進行中維持連線。
- **為什麼不用 Indicate**：一開始照標準規格訂閱 Indicate，實測發現無論怎麼調整連線時序（連線→訂閱成功→在裝置上量測），都等不到推播，且連線常常在等待期間就以各種原因斷開（`reason=0x08` 逾時、`att_status=0x1f` 訂閱失敗等，症狀不固定）。後來確認這台裝置**量測完才會開啟藍芽廣播**（跟額溫槍/血氧計一樣的行為模式），也就是連線建立時，量測早就結束了——Indicate 只會推播「訂閱之後才發生」的即時事件，不會補送連線前已經算完的舊結果，所以理論上等不到才是正常的。用手機 nRF Connect 直接對 `0x2A35` 按「Read」（不訂閱 Indicate）反而立刻讀到最新量測結果，證實這個特徵值可以直接讀取（雖然標準規格只定義 Indicate，這是裝置額外支援的行為）。改用 Read 之後不用再處理任何連線時序問題。
- 回應封包格式（照 Bluetooth SIG 官方標準 Blood Pressure Measurement 規格解析，**已用真實量測資料驗證正確**，見下方實測範例）：
  - `byte[0]` = flags（bit0=單位 mmHg/kPa、bit1=有無時間戳、bit2=有無脈搏、bit3=有無使用者ID、bit4=有無量測狀態）。
  - `byte[1..2]` = 收縮壓（IEEE-11073 SFLOAT，小端）。
  - `byte[3..4]` = 舒張壓（同上）。
  - `byte[5..6]` = mean arterial pressure（目前不取）。
  - 如果 flags bit1 有設，接著 7 bytes 是時間戳（目前程式碼會正確跳過，但沒有解析內容——**這台裝置本身的時鐘沒有校正過**，讀出來的時間戳完全不能信，跟量測數字本身無關，不用理會）。
  - 如果 flags bit2 有設，接著 2 bytes 是脈搏（SFLOAT）。
  - 目前**假設**單位一律是 mmHg（沒處理 flags bit0 為 kPa 的情況——正常情況下醫療血壓計幾乎都是 mmHg）。
  - 2026-08-04 實測範例：`06 7C 00 5B 00 00 00 E9 07 04 0B 09 37 00 57 00` → flags=0x06（有時間戳+有脈搏）、收縮壓=124、舒張壓=91、脈搏=87，跟裝置螢幕顯示的數字一致（時間戳解出來是錯的裝置內部時鐘，跟量測值無關）。
- **已驗證**：連線、標準服務/特徵值探索、直接 Read 取得量測結果、數值跟裝置螢幕一致。**血壓部分的核心流程已經打通**，但這次改用 Read 的版本還沒有實際燒錄進 Pico 測試過完整的「BLE 收到→WiFi 上傳」流程，見「7. 需要實習生測試的事項」第 1 點。

## 7. 需要實習生測試的事項（優先順序）

1. **【最優先】驗證血壓計走完整的「BLE 收到 → WiFi 上傳」流程。**
   - 協定格式已經用真實量測資料驗證過正確（見第 6.3 節），但改用「連線後直接 Read」取代「訂閱 Indicate 等推播」的版本還沒有實際燒錄測試過完整流程，優先確認這件事。
   - 讓 FORA D40 血壓計量一次血壓（隨時量都可以，不用管 Pico 有沒有連著）。
   - 觀察 log：應該會看到 `[BLE] matched FORA device ... (kind=3)` → `connected` → `[BLE] read value (N bytes): ...` → `parsed reading: type=4 ...`（收縮壓）、`type=5 ...`（舒張壓）、可能還有 `type=3 ...`（脈搏）。
   - 對照血壓計自己螢幕顯示的數字，確認解析出來的數字一致（時間戳不用管，裝置內部時鐘沒校正過，跟量測值無關）。
   - 確認這筆資料最後有成功上傳到測試網站（`test_server`），分類正確顯示 `systolic`／`diastolic`／`pulse_rate`。
   - **注意**：這款血壓計同一時間應該只能被一個中央裝置（Pico 或手機）連線，如果要同時用手機 nRF Connect 比對，記得先讓 Pico 那邊斷開/不要在旁邊搶著連線，否則手機會連不上、只能看到廣播訊號。

2. **血糖量測**：FORA D40 是血壓血糖二合一裝置，目前完全沒做。需要先用 BLE 掃描工具（例如 nRF Connect／LightBlue）連線確認血糖部分用的是哪個 service/characteristic（很可能是跟額溫槍/血氧計一樣的 Nordic LED/Button Service 自訂 pipe，需要現場量測比對 raw bytes 反推格式，做法可參考本文件第 6.2 節額溫槍/血氧計當初的除錯方式）。

3. **NTP 校時成功率**：目前逾時設定是 8 秒（`mode_upload.c` 的 `wall_clock_sync(8000)`），第一次測試（3 秒逾時）失敗過一次。需要多測幾次上傳，看 log 是不是穩定出現 `[TIME] NTP synced, epoch_ms=...`（成功）而不是 `[TIME] NTP sync timed out...`（失敗，會 fallback 顯示開機經過時間）。如果經常逾時，可能需要進一步拉長逾時或換一個更快的 NTP 伺服器（目前用 `pool.ntp.org`，可考慮換成 `time.cloudflare.com`）。

4. **24 小時等級耐用性測試**：這是這次交接的主要目的。讓裝置長時間連續運作（例如放床邊，斷續有真實裝置量測、斷續上傳），觀察：
   - 是否會卡死、無回應（需要重新插電才會恢復）。
   - flash 寫入次數多了之後是否穩定（待傳紀錄每次新增/上傳結果都會整份覆寫 flash，沒有 wear-leveling，見第 8 節第 4 點）。
   - WiFi 連線成功率、是否會遇到偶發性的 `NONET` 快速失敗（測試過程中觀察到過，原因未深入排查，見第 8 節第 3 點）。
   - 斷電測試：量測後、還沒上傳成功前故意拔電源，重開機後確認待傳資料還在（`storage_init()` 會從 flash 讀回），且會在下次連上 WiFi 時重試上傳。

5. **多裝置情境**：目前只驗證過「一次一種裝置在旁邊」，沒測過三種裝置同時在附近廣播、輪流量測的情境（連線順序、掃描優先權、是否會互相干擾）。

6. **不同 WiFi 環境**：目前主要測試環境是家用路由器（WPA2，SSID `SSTC-H100-2.4G`）跟 iPhone 個人熱點（要開「最大兼容性」才能用 2.4GHz，否則會連線失敗）。實際部署環境（醫院/病患家中）的路由器認證模式可能不同，程式碼已經做了多種認證模式輪流嘗試（見 `mode_upload.c` 的 `WIFI_AUTH_MODES_TO_TRY`），但沒有大量現場測試過。

## 8. 本次修好的重要 bug（給之後除錯時參考，不是待辦）

1. **【最關鍵】WiFi 連線「明明成功卻被誤判逾時砍斷重建」**：`cyw43_wifi_link_status()` 有時候不會準時回報 `CYW43_LINK_UP`，即使 lwIP 的 DHCP 早就 `dhcp_bind()` 拿到合法 IP 了。舊邏輯只信任這個狀態，結果白白等到逾時（原本設 30 秒），然後執行 `cyw43_arch_disable_sta_mode()+enable_sta_mode()`，把剛剛已經談成的連線整個砍掉重建，導致 WiFi 永遠連不上、一直重試。**修法**：改成直接檢查 `netif_is_up()` 且 `netif_ip4_addr()` 不是 `0.0.0.0`，不再只信任 `cyw43_wifi_link_status()`（見 `mode_upload.c`）。這個 bug 花了很長時間才抓到，因為表面症狀（卡在 JOIN 狀態、逾時失敗）長得很像是「認證失敗」或「DHCP 卡住」，靠打開 lwIP 的 `DHCP_DEBUG`/`NETIF_DEBUG` 逐行看 log 才發現 DHCP 其實真的成功了。
2. **AP_CONFIG 表單「填了 SSID 卻永遠收到空字串」**：真正原因是 HTTP headers 跟 body 常常被 TCP 拆成不同封包送達，舊邏輯看到 `\r\n\r\n` 就急著解析表單，那一刻 body 可能還沒送到、甚至完全是空的。**修法**：解析前先比對 `Content-Length` 跟目前已收到的 body 長度，不夠就繼續等下一段 TCP 資料（見 `mode_ap_config.c` 的 `parse_content_length()`）。
3. **`cyw43_wifi_scan()` 剛從藍牙模式切過來偶爾失敗（`-CYW43_EPERM`）**：STA 介面還沒真的啟用（`itf_state` 還是 0）。修法：`cyw43_arch_enable_sta_mode()` 加重試迴圈確認介面真的起來才掃描。
4. **待傳紀錄「上傳失敗後永遠不會重試」**：`storage_mark_uploaded()` 會把失敗的紀錄標成 `FAILED` 並保留（依註解原本的設計意圖），但 `storage_pending_records()` 篩選條件寫錯，只挑 `PENDING`，`FAILED` 的紀錄從此再也不會被撈出來重傳。修法：兩個函式的篩選條件都改成 `PENDING` 或 `FAILED`。
5. **兩種裝置（額溫槍/血氧計）共用同一份 GATT handle 快取**：導致連過額溫槍之後接著連血氧計會用錯 handle 查詢失敗（`att_status=0x0a`）。修法：依裝置種類（`fora_device_kind_t`）分開快取（見 `mode_ble_receive.c` 的 `s_handle_cache[]`）。
6. **idle timeout 從「進入 BLE_RECEIVE 模式」就開始倒數，不是從「收到資料」開始**：導致血壓計這種要 30-45 秒才會推播一次的裝置，量測進行中就被切去 UPLOAD、逼著斷線，永遠收不到資料。修法：加一個「這一輪有沒有收到過任何資料」的旗標，收到第一筆之前完全不檢查逾時（見 `mode_ble_receive.c` 的 `s_got_any_reading_this_session`）。
7. **待傳生理資料原本只存在 RAM，斷電就遺失**：現在會整份寫進 flash（`storage.c` 的 `persist_pending_records()`），開機時讀回，跟裝置設定分開存在不同的 flash sector。**注意：沒有 wear-leveling**，每次新增一筆或上傳結果都會整份覆寫一次那幾個 sector，寫入頻率高的話會較快耗損，正式量產前需評估升級成 littlefs。

## 8.1 2026-08-04 追加修好的 bug

8. **裝置量測完持續廣播期間反覆被重新連線、喚醒**：見第 8 節第 6 點的邏輯延伸，原本用「這一輪讀過了」的一次性旗標，只在單次 BLE_RECEIVE 執行期間有效，一旦切去 UPLOAD 再切回來就重置。改成用跨模式切換都持續有效的冷卻時間（`DEVICE_RECONNECT_COOLDOWN_MS`，目前 60 秒），拿到讀值後這段時間內完全不理會該種裝置的廣播。
9. **同類型資料重複累積、上傳好幾筆重複/過時的值**：跟上一點是同一個根因造成的症狀——裝置被反覆連線就會產生好幾筆幾乎一樣的紀錄。`storage_append_record()` 現在會先檢查有沒有同類型（`vital_type_t`）還沒上傳成功的舊紀錄，有的話直接覆蓋成最新這筆，而不是一直往陣列後面疊加。
10. **上傳的體溫數值小數位數異常**（例如顯示 `36.79` 而不是預期的 `36.8`）：`upload_api.c` 舊版 `format_value()` 用無條件捨去的方式手算小數（`(int)((value-whole)*100)`），浮點數表示誤差會讓 36.8 這種值被算成 36.79。改成先四捨五入到最接近的 0.1（體溫）或整數（SpO2/脈搏/血壓，這些本來就是整數）再格式化。

## 9. 已知限制 / 正式上線前必須處理

1. **TLS 憑證驗證目前是關閉的**（`upload_api.c` 的 `altcp_tls_create_config_client(NULL, 0)`，沒有帶入 CA 憑證，預設驗證模式是 `MBEDTLS_SSL_VERIFY_OPTIONAL`，等同不驗證）。測試階段可接受，正式上線前必須加上真正的伺服器憑證驗證，否則有中間人攻擊風險。
2. **上傳伺服器網址寫死在原始碼**（`UPLOAD_SERVER_HOST` 常數，目前指向 Cloudflare Quick Tunnel 的臨時測試網址，重開 `cloudflared` 就會換掉），`device_config_t` 也還沒有存這個欄位的地方。正式版需要讓伺服器位址可設定（例如加進 AP_CONFIG 設定頁）。
3. **熱點設定模式的 WiFi 密碼寫死**（`AP_PASSWORD "gateway123"`），正式版建議改成每台裝置唯一（例如印在裝置貼紙上）。
4. **上傳沒有任何認證機制**，任何人知道網址都能 POST 假資料進去。
5. 見第 8 節第 7 點：待傳資料的 flash 持久化沒有 wear-leveling。

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
├── test_server/             # 本機測試用的簡易上傳伺服器（Python）
└── src/
    ├── main.c
    ├── common.h             # 共用資料型別（device_config_t / vital_record_t / vital_type_t）
    ├── state_machine.c/.h
    ├── mode_boot_select.c/.h
    ├── mode_ap_config.c/.h
    ├── mode_ble_receive.c/.h
    ├── fora_protocol.c/.h   # 三種 FORA 裝置的協定解析，見第 6 節
    ├── mode_upload.c/.h
    ├── upload_api.c/.h
    ├── wall_clock.c/.h      # NTP 校時，boot-relative ms 換算成真實世界 epoch ms
    ├── led_status.c/.h
    └── storage.c/.h         # 設定值 + 待傳紀錄的 flash 持久化
```

## 11. 里程碑

1. ✅ 環境就緒。
2. ✅ 骨架就緒（狀態機 + LED + BOOTSEL 開機視窗）。
3. ✅ 熱點設定模式：已實機驗證手機連線、captive portal 自動彈出、表單送出、設定值讀回並帶入既有值。
4. ✅ BLE 接收模式：額溫槍、血氧計已完整驗證；血壓計連線/訂閱成功，量測資料解析待驗證。
5. ✅ 上傳模式：WiFi 連線 + HTTPS 上傳已完整驗證多次成功，含多種認證模式重試、DHCP 誤判 bug 修復、NTP 校時。
6. ⬜ 24 小時連續運作測試，觀察 flash 抹寫、記憶體、藍芽/WiFi 穩定度——**交接給實習生執行**。
