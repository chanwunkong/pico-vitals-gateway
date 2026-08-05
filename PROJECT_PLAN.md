# Raspberry Pi Pico W 生理訊號中繼裝置 — 專案計畫

> 這份文件是目前最新的狀態總覽與交接文件（2026-08-05 整理）。舊版內容（2026-07-31）已整合進來或標記為過時；如果內容跟程式碼有衝突，一律以程式碼為準，並回頭更新這份文件。

## 目前進度摘要（2026-08-05）

- ✅ 環境、骨架（狀態機、BOOTSEL 開機視窗、LED 燈號、flash 儲存）已完成並實機驗證。
- ✅ **熱點設定模式（AP_CONFIG）已實機驗證**：手機連上熱點會自動跳出設定頁（captive portal），可掃描附近 WiFi、選擇或手動輸入 SSID、填個案資訊，設定頁會帶入目前已存的值，密碼欄位留空 = 不變更。
- ✅ **BLE 接收模式支援三種 FORA 裝置，全部已完整實機驗證**：額溫槍、血氧計、血壓計都能正確量測並上傳。血壓計走的是反編譯官方 Windows 程式才確認的自訂協定（不是標準 Blood Pressure Service），見第 6.3/8.3 節；判重機制優先用裝置自己的量測時間戳（血壓計）、其餘裝置退回數值+時間窗口的經驗法則，見第 8.4 節。
- ✅ **WiFi 連線 + 上傳模式已完整打通並多次驗證成功**：連上真實家用路由器（WPA2）、透過 Cloudflare Tunnel 打 HTTPS 上傳到測試網站，資料正確顯示。上傳失敗的紀錄會標記 `FAILED` 並持續留在待傳佇列裡，只要待傳佇列非空、BLE_RECEIVE 閒置就會重新嘗試上傳，不需要額外的定時器；斷電也不會遺失（見下一點）。
- ✅ 待傳資料持久化到 flash（斷電/上傳失敗不會遺失，開機會自動讀回繼續重試）。
- ✅ 裝置量測時間優先用裝置自己的量測時間戳換算（目前只有血壓計有，見第 6.3/8.4 節），其餘裝置透過 NTP 換算成真實世界時間（校時失敗會 fallback 顯示開機經過時間，不會出錯）。
- ⬜ 血糖量測完全還沒開始（FORA D40 是血壓血糖二合一裝置，這次只做了血壓；畫面已預留欄位，見 12.6 節）。
- ⬜ 24 小時等級的耐用性測試完全沒做過——**這是這次交接給實習生的主要任務**。
- ✅ **電子紙顯示器 Phase 1+2 已完整實機驗證**：Waveshare Pico-ePaper-2.9 接線、驅動、四種模式畫面（AP_CONFIG／BLE_RECEIVE／UPLOAD／錯誤）都燒錄確認過，見第 12 節。個案姓名的中文顯示問題採方案 A（螢幕只顯示個案編號，見 12.3 節）。BLE_RECEIVE 的「Scanning」狀態文字改成帶時間戳的心跳（見 12.6 節），每 180 秒更新一次（比照資料手冊建議的刷新間隔下限）。
- 🔧 **AP_CONFIG 畫面加了 WiFi QR code**（原規劃 Phase 4，提前做，見 12.7 節）：手機掃碼直接跳出「加入 WiFi」提示連上 Pico 的設定熱點。**這部分還沒實機測試過**（沒有真的拿手機掃碼驗證）。
- ⬜ 電子紙 Phase 3（局部刷新排程）尚未開始，見 12.5 節。

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

裝置重新連線冷卻時間 `DEVICE_RECONNECT_COOLDOWN_MS`（`mode_ble_receive.c`）：某種裝置拿到讀值之後，這段時間內不會再重新連線同一種裝置，就算裝置還在廣播也不理它，讓它有機會真的休眠/閒置。這個冷卻時間**跨越 BLE_RECEIVE/UPLOAD 模式切換持續有效**（不是進 BLE_RECEIVE 模式就重置），這是修過的一個問題——原本每次重新進入 BLE_RECEIVE 都會重置「這輪讀過了」的記錄，如果裝置量測完還在持續廣播（常見行為），上傳一結束回到 BLE_RECEIVE 就會立刻被重新連線，導致裝置反覆被喚醒、還會產生一堆幾乎一樣的重複紀錄。

**2026-08-05 從單一共用值改成依裝置種類分開設定**（原本三種裝置共用 60 秒）：實測發現 FORA IR42 額溫槍看起來是「只要通電就持續廣播」，不是量測完才廣播一段時間就停，60 秒一到幾乎都還在原地廣播、馬上又被讀一次，讀到的值有時候還在緩慢漂移（例如同一顆額溫槍隔幾輪讀到 36.5 → 36.8 → 37.0 → 37.1），看起來比較像連續感應中，不是使用者又量了一次。額溫槍拉長到 **1 分鐘**（`FORA_DEVICE_THERMOMETER`），血氧計、血壓計目前沒觀察到同樣的問題，維持 **5 秒**（`FORA_DEVICE_OXIMETER`／`FORA_DEVICE_BLOOD_PRESSURE`）讓真正的新量測盡快被抓到。如果之後這兩種裝置也出現持續廣播/反覆喚醒的情況，需要回來個別調整對應的值。

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
- **（2026-08-05 最終定案，推翻本節之前所有「走標準 Blood Pressure Service」的記錄）**：這台裝置的血壓資料**不是**走標準 Bluetooth SIG Blood Pressure Service（`0x1810`/`0x2A35`）。曾經照標準規格訂閱 `0x2A35` 的 Indicate，實測連線後耐心等了 76 秒依然等不到任何推播——根本原因是這台裝置**量測完才開始廣播**（`s_current_kind` 判斷、使用者也證實「機器是量完血壓才開啟藍芽」），連線建立時量測早就結束，Indicate 只會推播「訂閱之後才發生」的新事件，不會補送舊結果，這是協定層面的理論限制，不是等待時間不夠長的問題。
- **真正的做法（反編譯官方 Windows 程式 `TaiDoc.BLE_PcLink`／`BLE_PCLink_Library.dll` 才確認，見 8.3 節反推方法）**：跟額溫槍/血氧計走**完全相同**的 Nordic LED/Button Service 自訂 pipe（`fora_protocol.h` 的 `FORA_SERVICE_UUID128`/`FORA_CHARACTERISTIC_UUID128`），用「寫指令、等 Notify 回應」一問一答地跟裝置要目前的量測記錄，不是被動等推播：
  - 指令格式（三種裝置共用）：`{ 0x51, cmd, p1, p2, p3, p4, 0xA3, checksum }`，checksum = 前 7 bytes 總和的低位元組（`fora_protocol_build_command()`）。
  - 取得「目前這一筆」記錄要送**兩次**指令：先送 `cmd=0x25`（`FORA_BP_CMD_GET_RECORD_PART_A`，索引 0=最新一筆）收到回應取 `byte[2..5]`；再送 `cmd=0x26`（`FORA_BP_CMD_GET_RECORD_PART_B`）收到回應再取 `byte[2..5]`；把兩次各 4 bytes 接成 8 bytes，交給 `fora_protocol_parse_reading(FORA_DEVICE_BLOOD_PRESSURE, ...)` 解析。
  - 8 bytes 私有格式（跟標準 SIG 格式完全不同）：`byte[4]`=收縮壓（整數 mmHg）、`byte[6]`=舒張壓（整數 mmHg）、`byte[7]`=脈搏（整數 bpm），其餘 bytes 是日期/心律不整旗標，目前沒有解析。完整欄位說明見 `fora_protocol.h` 開頭註解。
  - 想問裝置目前有幾筆記錄：`cmd=0x2B`（`FORA_BP_CMD_GET_RECORD_COUNT`），目前用不到，只記錄協定。
- **2026-08-05 已完整實機驗證**：連線→配對（見下方說明）→訂閱 Notify→送 part A→收回應→送 part B→收回應→解析→斷線→WiFi 上傳，整條流程跑了 3 次，log 範例：`51 25 05 35 a7 12 a5 0e` + `51 26 72 00 47 4e a5 23` → 收縮壓=114、舒張壓=71、脈搏=78，數值合理且三次連線都一致（因為裝置每次回的都是「目前最新一筆」，量測值本身沒變）。
- **這台裝置連線後需要先配對（`sm_request_pairing()`）才能訂閱/寫入成功**，額溫槍/血氧計不需要配對——這個差異目前照實測結果保留，原因未深究（可能是這台裝置的韌體對這個 characteristic 多加了 Security Mode 1 Level 2 的要求）。
- **裝置不會標記「已讀」、會持續廣播很久（2026-08-05 實測確認、已修好，見 8.4 節）**：裝置本身不會標記「這筆記錄已經被讀走」，每次連線都回傳一模一樣的「目前最新一筆」記錄。原本冷卻時間只有 5 秒，導致同一次量測被重複當成新資料上傳。**已修好**：(1) 判重邏輯改成優先比對裝置自己的量測時間戳（`fora_protocol_decode_measured_key()`，見下方說明），同一個時間戳保證是同一筆記錄；(2) 血壓計冷卻時間拉長到 4 分鐘（見 8.4 節，原因是使用者提供的線索：裝置在「傳送舊記錄」模式下設計是 3 分鐘無活動就自動關機，原本 60 秒的冷卻會一直重置這個計時器，讓裝置永遠關不了機）。
- **裝置自己的量測時間戳（2026-08-05 反推並接上，見 8.4 節）**：血壓記錄的 `byte[0..3]` 其實藏著裝置內部時鐘認證過的量測日期/時分（day/month/year/hour/minute，分鐘解析度），之前完全沒解析。現在 `fora_protocol_decode_measured_key()` 把它解成一個可比較的鍵值，用途有二：(1) 判重的依據（見上一點）；(2) 畫面/上傳資料的時間戳改用這個真正的量測時間，而不是「Pico 收到 BLE 通知的時間」（`fora_protocol_measured_key_to_datetime()`／`fora_protocol_measured_key_to_epoch_ms()`）。**實測驗證過兩次**：(a) 解碼出來的時分（18:39）跟量測當下的實際本地時間完全吻合；(b) 這次 NTP 校時剛好失敗，上傳的 `received_at_ms` 依然正確（因為完全不依賴 wall_clock），證明這個時間戳比原本靠 NTP 換算的方式更可靠。前提假設：裝置內部時鐘存的是本地時間（UTC+8，見 `common.h` 的 `LOCAL_UTC_OFFSET_SEC`），目前看起來成立。

## 7. 需要實習生測試的事項（優先順序）

1. ✅ **~~驗證血壓計走完整的「BLE 收到 → WiFi 上傳」流程~~（2026-08-05 已完整實機驗證，見第 6.3 節）**：連線→配對→兩段式取記錄→解析→上傳，跑了 3 次都成功，數值（收縮壓114/舒張壓71/脈搏78）跟裝置螢幕一致。

1.1 ✅ **~~血壓重複上傳同一筆記錄~~（2026-08-05 已修好並實機驗證，見第 6.3 節、第 8.4 節）**：改用裝置量測時間戳判重＋血壓計冷卻拉長到 4 分鐘。實測驗證：同一筆舊記錄重複收到時待傳筆數維持 0（不會重複上傳）；量到真正新的一次血壓（120/90/73，跟舊的 114/71/78 不同）時能正確被當成新資料上傳，判重沒有誤傷真正的新量測。

1.2 **【待辦，2026-08-05 使用者提出】血壓計自己的時鐘沒校時過的話，時間戳應該優先信任 NTP**：目前 `mode_upload.c`／`display_status.c` 只要 `device_measured_key != 0` 就無條件信任裝置回報的量測時間（見第 8.4 節第 18 點），完全沒有檢查這個時間戳合不合理。如果某台血壓計本身時鐘沒校時過（電池換過、從沒設定過、韌體預設值、時鐘飄移很多年），會直接把明顯錯誤的日期當成正確答案顯示/上傳，比退回用 Pico 自己 NTP 校時過的接收時間更不準、更有誤導性。需要補一個合理性檢查（例如跟 Pico 目前的 NTP 校時結果比較，如果裝置回報的時間跟現在差距超過某個合理範圍，例如幾天到一年，就判定裝置時鐘不可信、退回用 Pico 收到時間），目前還沒實作、也還沒設計檢查門檻。

2. **血糖量測**：FORA D40 是血壓血糖二合一裝置，目前**只做了畫面預留欄位**（`VITAL_TYPE_GLUCOSE`，見 12.6 節），協定完全沒有反推。需要先用 BLE 掃描工具（例如 nRF Connect／LightBlue）連線確認血糖部分用的是哪個 service/characteristic（很可能是跟血壓計同一套 Nordic LED/Button Service 自訂 pipe，用類似 `FORA_BP_CMD_GET_RECORD_PART_A/B` 的方式取值，需要現場量測比對 raw bytes 反推格式，做法可參考第 6.2/6.3 節當初的除錯方式）。

3. **NTP 校時成功率**：`mode_upload.c` 呼叫 `wall_clock_sync(8000)` 給 8 秒總預算。**2026-08-05 修過一個實際的正確性 bug**：原本的 `s_synced` 旗標只要成功過一次就永遠是 `true`，之後每次呼叫這個函式都會因為迴圈條件一開始就不成立而直接跳過，**等於開機後只有第一次真的會打 NTP，之後每次上傳都不會再真的對時**——RP2040 內建震盪器有漂移（量級大約一天幾秒），長時間 24/7 運作下去，時間戳會越來越不準，卻沒有任何自我修正機制。現在改成：
   - 每天最多真的打一次網路（`NTP_RESYNC_INTERVAL_MS`），不到一天就沿用現有基準點，不會每次上傳都浪費一次網路來回。
   - 伺服器改成清單輪流試（`pool.ntp.org` → `time.cloudflare.com`，做法比照 `mode_upload.c` 的 `WIFI_AUTH_MODES_TO_TRY`），單一伺服器沒回應就換下一個，不會整批直接放棄。
   - 如果已經有過一次成功校時、這次重新校時又失敗，會**保留舊的基準點**繼續換算時間（比完全退回 boot-relative 好），下次連上網路還會再試。
   
   還沒實機測試過這批改動：需要確認第一次開機時仍然能正常校時成功、且連續運作超過 24 小時後真的會觸發一次重新校時（看 log 有沒有在間隔超過一天後又出現 `[TIME] trying NTP server...`）。

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

8. **裝置量測完持續廣播期間反覆被重新連線、喚醒**：見第 8 節第 6 點的邏輯延伸，原本用「這一輪讀過了」的一次性旗標，只在單次 BLE_RECEIVE 執行期間有效，一旦切去 UPLOAD 再切回來就重置。改成用跨模式切換都持續有效的冷卻時間（`DEVICE_RECONNECT_COOLDOWN_MS`，依裝置種類分開設定：額溫槍/血壓計 60 秒起跳，血壓計後來又拉長到 4 分鐘，見第 8.4 節第 17 點；血氧計目前維持 5 秒），拿到讀值後這段時間內完全不理會該種裝置的廣播。
9. **同類型資料重複累積、上傳好幾筆重複/過時的值**：跟上一點是同一個根因造成的症狀——裝置被反覆連線就會產生好幾筆幾乎一樣的紀錄。`storage_append_record()` 現在會先檢查有沒有同類型（`vital_type_t`）還沒上傳成功的舊紀錄，有的話直接覆蓋成最新這筆，而不是一直往陣列後面疊加。
10. **上傳的體溫數值小數位數異常**（例如顯示 `36.79` 而不是預期的 `36.8`）：`upload_api.c` 舊版 `format_value()` 用無條件捨去的方式手算小數（`(int)((value-whole)*100)`），浮點數表示誤差會讓 36.8 這種值被算成 36.79。改成先四捨五入到最接近的 0.1（體溫）或整數（SpO2/脈搏/血壓，這些本來就是整數）再格式化。

## 8.2 2026-08-05 追加修好的 bug（電子紙上線後排查上傳/顯示問題時發現）

11. **上傳其實成功了，卻一直被裝置自己判斷成失敗**：`upload_api.c` 的 `on_recv()` 原本只看「這一次」callback 收到的前 64 bytes 裡有沒有 "200"，但 HTTP 回應可能被 TLS record／pbuf 邊界切成好幾段送達，如果狀態列 `HTTP/1.1 200 OK` 剛好被切成兩段（例如這次只收到 `HTTP/1.1 2`），兩次個別比對都找不到完整的 "200"，導致伺服器端明明已經正確收到資料、回了 200，裝置這邊卻一直回報失敗、進而不斷重試上傳同一批資料。**這不是猜測，是實測抓到的**：2026-08-05 debug 時比對 `test_server/server.log`（有收到、回 200）跟裝置序列埠 log（一直印 `result: failed`）才發現。修法：改成把收到的內容累積進一個跨 callback 持續存在的 buffer（`upload_ctx_t.header_buf`），比對累積後的內容，不再只看單次片段。同時把失敗時的除錯 log 從「印出片段」改成「印出目前為止累積收到的完整內容」。
11.1 **（同一天再追一層）修完上面那個之後還是失敗，這次是真的失敗**：伺服器回 `400 Bad Request`。加了「印出實際送出的完整請求」這行 debug log 之後才抓到——`patient_id` 這個欄位裡混進了幾個控制字元（實測看到 `\x01`、`\x06`、`\x14`，混在使用者原本輸入的 "00" 中間，很可能是很久以前某次測試殘留在 flash 裡的資料，成因不明），沒有跳脫就直接塞進 JSON 字串，導致伺服器端 `json.loads()` 解析失敗（`Invalid control character`）。`patient_id` 跟 SSID/密碼一樣，是 AP_CONFIG 表單的自由輸入欄位，**內容完全不受韌體控制**，跟 `display_status.c` 處理 WiFi QR code 裡的 SSID/密碼同一個道理，塞進任何有格式規則的地方（JSON、QR code、EPD 的 ASCII 字型）之前都必須先跳脫/過濾，不能假設內容乾淨。修法：`upload_api.c` 新增 `append_json_escaped()`，把 `patient_id` 塞進 JSON 前跳脫 `"`、`\`、控制字元（`\u00XX`），`build_request()` 改用這個函式組 `patient_id` 欄位。
12. **UPLOAD/AP_CONFIG/錯誤畫面顯示完之後，回到 BLE_RECEIVE 卻可能永遠不刷新，面板卡在舊畫面**：`display_status_show_upload()` 這幾個函式是直接畫、直接刷新，不會去更新 BLE_RECEIVE 那邊「上次真的畫了什麼」的快照。如果從 UPLOAD 回到 BLE_RECEIVE 時，這次 BLE_RECEIVE 的內容剛好跟切走前最後一次畫的一模一樣（例如都是 3 筆待傳、都是 "Scanning..."），`display_status_poll()` 的內容比對會覺得「沒有變化」而不刷新——但面板上其實還停在 UPLOAD 畫面，使用者看到的是「卡在上傳中」，以為裝置不在 BLE 接收模式。修法：加一個 `s_ble_screen_is_current` 旗標，AP_CONFIG／UPLOAD／錯誤／開機驗證畫面顯示時清成 false，強制下一次 BLE_RECEIVE 的 `display_status_poll()` 至少刷新一次，不管內容比對結果如何。
13. **`mode_ble_receive.c` 的 debug log 也有一份跟 upload_api.c 同樣的浮點數截斷 bug**（第 8 節第 10 點修的是上傳 JSON 那份格式化邏輯，這裡是另一份只給 `[BLE] parsed reading:` 這行 log 用的，當時沒有一起修到）：36.8 這種值印成 36.79，數值本身沒錯，只是 debug log 顯示的精度看起來怪怪的，容易誤導排查方向。改成跟 `upload_api.c` 一樣的四捨五入到小數點後 1 位。

## 8.3 2026-08-05 血壓計協定反推方法（給以後反推血糖協定參考）

14. **血壓計的真正協定是靠反編譯官方 Windows 程式找到的，不是靠猜或靠 nRF Connect 觀察**：官方電腦端程式 `FORA Health Care Management System_BLE`（使用者安裝在 `%APPDATA%\FORA Health Care Management System_BLE\`）裡的 `BLE_PCLink_Library.dll`（`TaiDoc.BLE_PcLink` 命名空間）用 ILSpy 反編譯後，直接看到組指令/解析回應的原始碼，才確認：(a) 血壓計走的是自訂 Nordic LED/Button Service pipe，不是標準 Blood Pressure Service；(b) 指令格式跟額溫槍/血氧計共用同一套 `{0x51, cmd, ..., 0xA3, checksum}`；(c) 取記錄要送兩次指令（`0x25`+`0x26`）才能拼出完整一筆。
    - 反編譯工具鏈：`dotnet tool install --global ilspycmd --version 8.2.0.7535`（**必須釘住版本**，不帶版本的 `--version latest` 會因為套件本身缺 `DotnetToolSettings.xml` 而安裝失敗）+ `Microsoft.DotNet.Runtime.6`（`ilspycmd` 執行期依賴 .NET 6，跟系統原本裝的其他版本無關，要另外裝）。
    - 這個方法之所以能用，是因為使用者本身有安裝官方 Windows 端程式且同意花時間裝一整套 .NET SDK/反編譯工具（約 1GB）——**這不是每次遇到協定不明的裝置都能複製的路徑**，前提是廠商有提供電腦端程式、且該程式沒有額外加殼/混淆。如果之後要反推血糖協定，官方程式應該也有對應的血糖解析邏輯，同一支 DLL 裡应该能找到（`BLE_PCLink_Library.dll` 已經在使用者電腦上，不需要重新反編譯，直接找血糖相關的 class/method 即可）。

## 8.4 2026-08-05 血壓計協定上線後、實機重複測試發現並修好的 bug

血壓計協定（見 6.3/8.3 節）第一次燒錄實測成功後，接著做了好幾輪「同一台裝置故意重複觸發廣播」的測試，陸續發現並修好以下問題：

15. **同一筆血壓記錄被重複當成新資料上傳**：見第 6.3 節「裝置不會標記已讀」的說明。修法是新增 `fora_protocol_decode_measured_key()`，把血壓記錄 `byte[0..3]` 的日期/時分解碼成一個可比較的鍵值，`storage_append_record()` 判重時如果雙方都有這個鍵值就直接比對是否相等（不用再猜時間窗口）；額溫槍/血氧計的回應封包沒有這種時間戳（鍵值恆為 0），這種情況才退回用「數值相同+10 分鐘內」的經驗法則（`DUPLICATE_SUPPRESS_WINDOW_MS`）。**實機驗證**：同一筆舊記錄（114/71/78）連續 3 次重新連線都正確判定為重複、待傳筆數維持 0；量到真正新的一次血壓（120/90/73）時正確被當成新資料、待傳筆數變成 3。
16. **判重生效後，`mode_ble_receive_run()` 的 idle timeout 邏輯還是會切去 UPLOAD 白跑一趟 WiFi**：原本的邏輯是「距離上次收到讀值超過 N 秒就切去 UPLOAD」，不管待傳佇列裡實際上有沒有東西（因為判重會讓收到讀值但不放進佇列）。修法：切換前先檢查 `storage_pending_count() > 0`，沒有東西要傳就重置這一輪的旗標、留在 BLE_RECEIVE 繼續掃描，不會為了「連一次 WiFi 確認看看」白跑。
17. **血壓計冷卻時間從 60 秒又拉長到 4 分鐘**：拉到 60 秒（比照額溫槍）之後，實測發現血壓計還是完全不會自己關機（量測完超過 30 分鐘依然持續廣播、回應連線）。使用者提供關鍵線索：這台裝置在「傳送舊記錄」模式下設計是 3 分鐘無活動就會自動關機，Pico 每 60 秒重新連線一次很可能持續重置這個計時器，導致裝置永遠撐不到 3 分鐘門檻。改成 4 分鐘（比 3 分鐘門檻多留 1 分鐘餘裕）。判重邏輯不依賴冷卻時間長短，拉長不影響正確性，只是「偵測到真正新量測」最慢會慢 4 分鐘。**這個假說目前還沒有實機確認**（需要觀察拉長之後裝置是不是真的會停止廣播/斷電），是本節唯一還沒驗證完成的項目。
18. **血壓計顯示/上傳的時間戳原本是「Pico 收到 BLE 通知的時間」，不是「裝置實際量測的時間」**：兩者理論上可能不同（裝置量完到被 Pico 連上讀到資料之間可能有延遲），且原本的邏輯依賴 NTP 校時才能換算成真實時間、NTP 失敗時間戳就不準。修法：`fora_protocol_measured_key_to_datetime()`／`fora_protocol_measured_key_to_epoch_ms()` 把裝置自己的量測時間戳解碼、換算成 epoch ms，`display_status.c` 畫面跟 `mode_upload.c` 上傳都優先使用這個值（有值才用，額溫槍/血氧計沒有這個資訊，照舊用 Pico 收到時間）。**實機驗證**：(a) 解碼出來的時分（18:39）跟量測當下的實際本地時間完全吻合；(b) 剛好遇到一次 NTP 校時失敗（`NTP sync timed out on all servers`），上傳的 `received_at_ms` 依然正確顯示裝置量測時間，證明不依賴 wall_clock 也能正確運作。換算邏輯假設裝置內部時鐘存的是本地時間（UTC+8，`common.h` 的 `LOCAL_UTC_OFFSET_SEC`），目前實測看起來成立。**已知風險（待辦，見第 7 節第 1.2 點）**：目前完全信任裝置回報的時間戳，沒有做任何合理性檢查——如果某台血壓計本身沒校時過（電池換過、從沒設定過、時鐘飄移），會直接把錯誤的時間當成正確答案上傳，比退回用 Pico 收到時間（至少有 NTP 校正過）更不準。
19. **`VITAL_TYPE_PULSE_RATE` 被血氧計跟血壓計共用，導致跨裝置判重誤判**：`storage.c` 判重原本只比對 `vital_type_t`，不管是哪種裝置回報的。實測抓到症狀：血壓計量到脈搏 73（有装置時間戳）覆蓋了 `s_last_reading[PULSE_RATE]` 之後，血氧計下一次回報脈搏 77（跟血壓計的 73 不同）被拿去跟血壓計的 73 比較，數值不同、誤判成新資料而多傳一筆（表面症狀是「多傳」，但反過來如果剛好數值相同，會誤判成重複而**漏傳血氧計真正的新讀值**，是更嚴重的方向）。修法：`common.h` 的 `vital_record_t` 新增 `source_kind` 欄位（記錄是哪種裝置回報的，`fora_protocol_parse_reading()` 負責填入），`storage_append_record()` 判重時額外要求 `source_kind` 也要相同才算重複，不同裝置種類回報同一種 `vital_type_t` 一律視為不同來源、不比對。
20. **電子紙「Scanning」心跳刷新間隔改成 180 秒**：原本 `mode_ble_receive_run()` 主迴圈裡的心跳刷新（讓沒有裝置連線活動時，畫面時間戳依然定期前進，見第 12.6 節「最後掃描時間」設計）用 60 秒，使用者指出 Waveshare 資料手冊建議刷新間隔至少 180 秒（見 12.5.1 節），既然這是唯一一個「定期、沒有實際新事件也會觸發」的刷新來源，沒有理由讓它成為唯一違反建議值的地方，改成 180 秒。跟 12.5.1 節「180 秒建議值不嚴格遵守」的決定不衝突：那個決定是說「真的有新資料/狀態要顯示時不要延遲」，這裡剛好相反——沒有新事件、純粹是心跳，本來就沒有「越快越好」的理由。

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
    ├── display_status.c/.h # 電子紙顯示器封裝，見第 12 節
    └── storage.c/.h         # 設定值 + 待傳紀錄的 flash 持久化
```

## 11. 里程碑

1. ✅ 環境就緒。
2. ✅ 骨架就緒（狀態機 + LED + BOOTSEL 開機視窗）。
3. ✅ 熱點設定模式：已實機驗證手機連線、captive portal 自動彈出、表單送出、設定值讀回並帶入既有值。
4. ✅ BLE 接收模式：額溫槍、血氧計、血壓計三種裝置都已完整實機驗證（血壓計走反編譯確認的自訂協定，見第 6.3/8.3 節）。血糖協定尚未開始（見第 7 節第 2 點）。
5. ✅ 上傳模式：WiFi 連線 + HTTPS 上傳已完整驗證多次成功，含多種認證模式重試、DHCP 誤判 bug 修復、NTP 校時、失敗自動重試＋斷電持久化（見第 8.4 節）。
6. ⬜ 24 小時連續運作測試，觀察 flash 抹寫、記憶體、藍芽/WiFi 穩定度——**交接給實習生執行**。
7. ✅ 電子紙顯示器（Waveshare Pico-ePaper-2.9）Phase 1+2 都已燒錄實機驗證：接線/文字顯示正常，四種模式（AP_CONFIG／BLE_RECEIVE／UPLOAD／錯誤）真實資料畫面都測過，詳見第 12 節。🔧 Phase 4 提前做的 AP_CONFIG WiFi QR code（12.7 節）程式碼已寫完、編譯過關，**還沒實機掃碼測試**。⬜ Phase 3（局部刷新排程）尚未開始。

## 12. 電子紙顯示器（Waveshare Pico-ePaper-2.9）規劃

> 2026-08-05 追加。目前裝置只靠 LED 燈號（見第 4 節）當作使用者能看到的唯一回饋，燈號規則需要背起來才看得懂，個管師/家屬完全看不出裝置在幹嘛。接上這片 296×128 電子紙後，目標是讓螢幕變成「不用懂技術也看得懂」的儀表板，LED 保留當底層、隨時看得到的心跳/錯誤指示（螢幕更新慢，LED 補足即時性），兩者不是取代關係。

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
- **2026-08-05 決定：採用方案 A**——螢幕上只顯示個案編號（`patient_id`）取代姓名，姓名全名留在上傳的資料裡，不上螢幕。
  - **注意（Phase 2 實作時必須處理，不然可能當機）**：`mode_ap_config.c` 的表單裡 `patient_id`（`pid` 欄位）跟 `patient_name` 一樣是**沒有限制輸入內容的自由文字欄位**，使用者一樣可能填中文進去，不保證一定是英數字。`GUI_Paint.c` 的 `Paint_DrawChar()` 是用 `(char - ' ')` 直接算 flash 位址偏移量，如果收到 UTF-8 多位元組的中文字（每個位元組被當成一個獨立字元），算出來的偏移量可能是很大的負數/超出字型表範圍，輕則亂碼、重則讀到無效的 flash 位址讓裝置當機。實作 Phase 2 顯示 `patient_id` 時，**畫面顯示前要先過濾成只保留可印出的 ASCII 字元（0x20–0x7E），遇到其他 byte 就跳過或截斷**，不能直接把 `patient_id` 原始字串丟給 `Paint_DrawString_EN()`。
  - 曾考慮過的替代方案（已否決，記錄備查）：方案 B 是拉一份完整繁體中文點陣字型進 `epd/Fonts/`，但檔案通常上百 KB 到數 MB，RP2040 只有 2MB flash 且已被 WiFi/BTstack/mbedTLS 佔掉不少，風險較高；方案 C 是另外收集羅馬拼音/暱稱欄位，多一道使用者輸入步驟。

### 12.4 軟體模組設計

`src/display_status.c/.h`，跟 `led_status` 平行、風格一致：

```c
void display_status_init(void);
void display_status_show_boot_test(void);   // Phase 1 驗證畫面，已實機驗證

void display_status_show_ap_config(const char *ap_ssid, const char *ap_password,
                                    const device_config_t *existing_config);
void display_status_show_upload(const char *ssid, const char *result_text);
void display_status_show_error(const char *message);

void display_status_set_ble_receive(const device_config_t *config, const char *status_text);
void display_status_poll(void);   // 非阻塞，只給 BLE_RECEIVE 的主迴圈呼叫
```

AP_CONFIG／UPLOAD／錯誤畫面的內容在該模式執行期間變動不頻繁，`mode_ap_config.c`／`mode_upload.c` 在幾個關鍵時間點直接呼叫對應的 `show_xxx()`（直接畫、直接刷新），不需要輪詢。BLE_RECEIVE 不一樣：它的主迴圈是持續數十秒到數分鐘的緊迴圈，內容會持續變動，所以拆成 `set_ble_receive()`（只更新「想顯示的內容」，便宜）+ `poll()`（`mode_ble_receive.c` 主迴圈每輪呼叫，內部比對「這次的內容」跟「上次真的畫到螢幕上的內容」，只有真的不一樣才觸發一次全刷）——比對刻意不包含任何「距今 N 分鐘」這種會隨時間漂移的文字，只比對原始數值/時間戳/筆數/狀態文字，避免時間流逝本身觸發刷新風暴（詳見 12.5 節）。

### 12.5 全刷／局部刷新與非阻塞的取捨

- 全刷（`EPD_2IN9_V2_Display`/`Display_Base`）約 3 秒、會整片閃黑再回來，但沒有殘影；局部刷新（`EPD_2IN9_V2_Display_Partial`）約 0.6 秒不會閃，但疊代次數多了畫面會有殘影，需要定期強制全刷清掉。**Phase 2 目前還是只用全刷**，局部刷新留給 Phase 3。
- **`EPD_2IN9_V2_ReadBusy()` 是阻塞式忙等**（`while (BUSY==1) sleep_ms(50)`），全刷一次會讓主迴圈卡住 2-3 秒。跟 `led_status_poll()` 的設計理由一樣，這段時間主迴圈完全停擺會影響 BLE GATT 事件處理／BTstack run loop 的即時性。解法是 `display_status_poll()` 內部做「內容真的變了才刷新」的比對（見上面 12.4 節），而且比對刻意不含會隨時間漂移的「X 分鐘前」文字——**代價是畫面上的「X min ago」顯示的是「上次刷新當下」算出來的，不是即時的**，要有新事件（新讀值、待傳筆數變化、狀態文字改變）畫面才會真的更新一次。這是刻意的取捨：全刷要 3 秒，如果為了讓文字即時而定時刷新，BLE 迴圈的即時性會被拖累；e-paper 本來就是「一眼瞄過去看大概」的裝置，不需要秒級精確。
- 生理讀值本身天生就不會太頻繁（同一種裝置有 60 秒冷卻時間、不同裝置量測要數秒到數十秒），加上一次量測（例如血壓計同時回傳收縮壓+舒張壓+脈搏）會在同一個事件循環內連續呼叫多次 `storage_append_record()`、`poll()` 下一輪才執行一次，所以不需要額外加節流計時器，內容比對本身就已經天然稀疏。

#### 12.5.1 面板保護：睡眠與刷新頻率限制（硬體規格書要求，2026-08-05 補上，同日調整過一次）

Waveshare 資料手冊列了幾點面板保護要求，**其中「不能長時間通電/必須睡眠」是硬性規定（違反會造成不可逆的實體損壞）**，其他幾點是建議值：

- 電子紙面板**不能長時間維持通電/高電壓狀態**（硬性）：不刷新的時候必須進睡眠模式（或斷電），不然膜片會壞掉、修不好。
- 刷新間隔建議至少 180 秒（建議值）。
- 建議至少每 24 小時要刷新一次，就算內容沒變也要刷（建議值，避免長時間靜態顯示造成殘影/老化）。
- 長期不使用的話，面板要先刷成全白再收起來存放（實體庫存/備品管理層面的事，跟韌體邏輯無關，這裡只是記錄需求，沒有做對應的韌體功能）。

**2026-08-05 決定：180 秒建議值不嚴格遵守，以使用方便性為原則**——BLE_RECEIVE 收到新讀值、UPLOAD 顯示結果都是使用者/個管師想立刻看到的狀態，硬性延遲 3 分鐘才顯示，體驗上得不償失。「不能長時間通電」這條硬性規定則不受影響，照樣嚴格遵守。目前 `display_status.c` 的實際作法（`end_frame_and_refresh()` 是唯一把關的地方，四種畫面都經過它）：

1. **每次刷新都是「喚醒 → 畫 → 睡眠」一整套，沒有例外**：`EPD_2IN9_V2_Init()`（內部會先做硬體 Reset，同時也是喚醒深度睡眠的標準程序）→ `EPD_2IN9_V2_Display_Base()` → `EPD_2IN9_V2_Sleep()`。面板在兩次刷新之間永遠是睡眠狀態，不會停在「醒著但沒在畫」的高電壓狀態——這一條沒有被上面的決定影響。
2. **沒有 180 秒下限**：呼叫端要求刷新就會真的刷新，不會被延遲。
3. **保留 24 小時強制刷新**：`display_status_poll()` 裡，就算內容完全沒變，距離上次刷新超過 24 小時也會觸發一次刷新（AP_CONFIG／UPLOAD 執行時間本來就短，用不到這個機制，只有 BLE_RECEIVE 這個 24/7 常駐模式需要）。
4. `end_frame_and_refresh()` 回傳值目前恆為 `true`（沒有任何情況會跳過），但介面還是設計成回傳 bool——`display_status_poll()` 那邊「沒刷新成功就不更新快照」的邏輯還在，之後如果又要加別的跳過條件（例如偵測到面板故障、或某天又想調整節流策略）不用改呼叫端。
5. **（2026-08-05 再次調整）`main.c` 開機時不再自動呼叫 `display_status_show_boot_test()`**：拿掉 180 秒下限之後，這通呼叫本身已經不會再拖慢後面畫面的顯示時間，但重新考慮後，這個畫面（英文技術字樣「Pico Vitals Gateway / e-Paper OK」+ 外框）對個管師/家屬來說沒有意義，只是開發驗證接線用的，決定回到不自動顯示、需要時手動呼叫確認硬體即可。函式本身還在 `display_status.c`，只是沒有從 `main.c` 呼叫。
6. `display_status_init()` 只做 SPI/GPIO 跟 framebuffer 初始化，**不會**呼叫 `EPD_2IN9_V2_Init()/Clear()`（那本身就是一次刷新）——面板的硬體初始化延後到第一次真的要刷新畫面時，由 `end_frame_and_refresh()` 觸發，避免開機時做兩次背靠背的刷新（一次來自 init 的 Clear、一次來自緊接著的 boot test 畫面）。

**尚未驗證**：這批改動還沒有實機測試過（例如確認 BLE_RECEIVE 連續收到好幾筆讀值時，畫面是不是每次都有即時更新；或修改 `MAX_REFRESH_INTERVAL_MS` 成短時間驗證 24 小時強制刷新邏輯有沒有真的觸發）。

### 12.6 畫面內容（Phase 2 已實作並實機驗證，2026-08-05）

| 模式 | 畫面內容 | 對應函式 |
|---|---|---|
| AP_CONFIG 熱點設定 | 熱點 SSID/密碼文字、目前已存的個案編號（ASCII 過濾過，沒設定過就顯示 `(unset)`）、操作提示、WiFi QR code（見 12.7 節） | `display_status_show_ap_config()` |
| BLE_RECEIVE | 個案編號（方案 A）、目前狀態（`Scanning (MM/DD HH:MM)`，見下方說明）、體溫/血氧/脈搏/**血糖（欄位保留，協定未實作，永遠顯示 `-- (never)`）**/血壓各自最後一筆數值＋時間戳（已校時顯示 `MM/DD HH:MM`絕對時間，血壓計顯示的是**裝置自己的量測時間**而非 Pico 收到時間，見第 6.3/8.4 節；未校時且無裝置時間戳顯示 `unsynced,+Nm`）、**最後一次成功上傳時間**、待上傳筆數 | `display_status_set_ble_receive()` + `display_status_poll()` |
| UPLOAD | WiFi SSID + 目前階段/結果文字（`Connecting...`/`Success (N records)`/`Failed, will retry`） | `display_status_show_upload()` |
| 錯誤 | 一句英文錯誤訊息，取代難記的三連閃燈號 | `display_status_show_error()` |

**「Scanning」狀態文字的心跳設計**：故意不用靜態的 `"Scanning..."`——電子紙斷電/當機時畫面會凍結在最後一次刷新的內容，如果狀態文字本身不含時間資訊，使用者沒辦法從畫面分辨「裝置還活著、只是沒掃到裝置」跟「裝置已經當機」。`update_scanning_status()` 組出帶時間戳的狀態文字，並在 `mode_ble_receive_run()` 主迴圈裡每 180 秒（2026-08-05 從 60 秒調整，比照 Waveshare 資料手冊建議的刷新間隔下限，見 12.5.1 節）定期重新呼叫一次，讓時間戳在沒有裝置連線活動時依然會前進。**尚未實機視覺驗證**：程式邏輯已審視過、序列埠 log 能間接佐證運作時機（BLE_STATE_SCANNING 期間確實會定期呼叫），但這次測試過程中裝置一直有連線活動（額溫槍/血氧計/血壓計輪流被連上），沒有真的空出一段安靜超過 180 秒的時間去肉眼確認面板上的時間戳有沒有正確前進，之後需要找一段裝置都不在旁邊廣播的時間補驗證。

**尚未做的**：局部刷新排程（減少全刷閃爍/縮短刷新時間）還沒做，見上面 12.5 節。

### 12.7 AP_CONFIG 的 WiFi QR code（2026-08-05 加做，原規劃 Phase 4 提前）

- 手機相機掃到 `WIFI:T:WPA;S:<ssid>;P:<password>;;` 這個特定前綴的字串會自動跳出系統內建的「加入 WiFi」提示——這是業界慣例（源自 ZXing），**QR code 本身沒有什麼特殊格式/模式**，跟顯示純文字/網址的 QR code 用的是同一套編碼方式，差別只在字串內容。SSID/密碼裡如果出現 `\`、`;`、`,`、`:`、`"` 這幾個字元，依慣例要加反斜線跳脫（`display_status.c` 的 `append_escaped_wifi_field()`）——目前 `AP_SSID`/`AP_PASSWORD` 是寫死字串沒有這些字元，但先做完整，避免以後密碼變成可設定/隨機產生時漏掉。
- 這個 QR code 編碼的是**熱點本身**的 SSID/密碼（`AP_SSID`/`AP_PASSWORD`，讓手機能連上 Pico 的設定用熱點），**不是**個案要接的目標 WiFi（`device_config_t.wifi_ssid`/`wifi_password`，那組帳密是使用者在熱點頁面的表單裡填的，不會出現在任何 QR code 上）。
- QR code 編碼器：從 Nayuki 的 `QR-Code-generator`（`c/qrcodegen.c`/`.h`，MIT 授權）複製進專案根目錄 `qrcode/`，純 C89、沒有外部依賴（只用標準函式庫），沒有改動任何一行。呼叫 `qrcodegen_encodeText()`，ECC 等級用 MEDIUM（可以容忍約 15% 資料損毀，兼顧掃描容錯率與 QR 大小），版本上限給 10（`qrcodegen_BUFFER_LEN_FOR_VERSION(10)` 對應的 buffer 只有 408 bytes，對這種 40~50 bytes 的短字串綽綽有餘，實際會落在 version 2~3 左右，遠用不到上限）。
- 畫在 AP_CONFIG 畫面右側（`draw_qr_code()`，把 QR 的每個 module 依比例放大成好幾個實際像素畫上去，四周留白靠 `Paint_Clear(WHITE)` 清出來的背景自然滿足，不用額外處理），文字（SSID/密碼/個案編號/操作提示）留在左側，兩者之間留了足夠間距。
- 還沒有拿真的手機在真的裝置上掃過（跟 Phase 2 一樣，這批程式碼還沒燒錄實測）。
