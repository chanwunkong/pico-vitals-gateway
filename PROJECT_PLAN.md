# Raspberry Pi Pico W 生理訊號中繼裝置 — 專案計畫

> 狀態總覽與交接文件（2026-08-06 整理）。內容跟程式碼有衝突時一律以程式碼為準，並回頭更新這份文件。

## 2026-08-28 交接 prompt（換電腦接續 Rightest GM700SB 除錯用）

> 這一段是換電腦/換 AI session 接續開發時，直接貼給新 session 的起手 prompt。完整的除錯過程、每一輪實機測試的發現，見下面第 6.6 節，這裡只放接手當下需要的濃縮版。

```
接續 pico-vitals-gateway 專案，正在加入 Bionime Rightest GM700SB 血糖機支援。
Git 分支 feature/rightest-gm700sb（從 master 分出來，master 沒動過，這個分支已經
commit 並 push 上 GitHub）。先讀 PROJECT_PLAN.md 第 6 節（尤其是 6.6 節）跟
src/rightest_protocol.h/.c、src/mode_ble_receive.c 裡 Rightest 相關的部分，
了解完整協定/架構決策再繼續。

目前卡住的問題：裝置能正常掃描到、連線、探索到 FEE0/FEE1/FEE2/FEE3、訂閱
Notify、收到裝置自動推播的 meter-ID（16 bytes，沒有協定文件說的分包表頭，
已經處理）、寫入 PCL 開啟指令也拿到 ATT 層成功確認。但接下來**任何**指令
（型號查詢、讀取記錄總數 0xB0 0x61 0x00 0x00）寫進 FEE3 之後，ATT 層一樣
回成功確認，裝置卻從來沒有透過 FEE2 回傳任何 Notify 內容，17 秒後連線逾時
斷線，反覆同樣的循環。裝置螢幕全程顯示橫槓左右移動的動畫——查過官方說明書
確認這個動畫代表「正在傳輸資料」，不是卡住畫面，但我們這邊完全收不到資料。

配對本身另外卡過幾輪（裝置需要在裝置本體手動按鍵確認配對，不是純軟體
Just Works；BTstack 的配對金鑰儲存不會被韌體重新燒錄清掉，跟裝置端的配對
狀態對不上時會卡在等配對回應、整個沒有任何回應）。這次接手前使用者已經在
裝置端把配對記錄清空，最近一次測試是配對不需要就直接卡在讀記錄那步，代表
配對已經不是主要卡點，卡點在「裝置收到指令但沒有實際回應」。

還沒試過、值得先試的兩個方向：
1. 協定文件架構圖顯示 GM700SB 是「藍牙模組」+「血糖機主控 MCU」兩顆晶片
   透過 UART 內部連接，ATT 層寫入成功只代表藍牙模組收到，不代表主控 MCU
   真的處理完、準備好接指令——目前 PCL 開啟拿到 ATT 確認後幾乎立刻送下一
   個指令，可能太快。可以試著在送指令前加一點延遲（例如 0.5~1 秒）。
2. 如果使用者手機是 Android 且有開發者選項，可以請他們開「藍牙 HCI 監聽
   記錄」（btsnoop log），用官方「瑞特血糖守護寶」App 完整跑一次同步流程
   後把記錄匯出分析，直接看官方 App 實際的指令/間隔，比用猜的準確很多。

實機測試流程（燒錄/監看序列埠的細節見第 0 節）：改完 code 要
`cmake --build build`，裝置需要使用者手動進 BOOTSEL 模式（按住 BOOTSEL
同時按 RUN 鍵，或拔插 USB）才能複製 .uf2 燒錄，燒錄跟按鍵都需要使用者
在現場配合操作，這邊沒辦法自動化。序列埠是 USB CDC（COM port，因電腦而
異），用 .NET SerialPort 監看記得手動設 DtrEnable/RtsEnable 為 true，
不然收不到任何輸出（TinyUSB CDC 的已知行為）。
```

## 目前狀態摘要

**已完成、已實機驗證：**

- 環境、骨架（狀態機、開機自動進 AP_CONFIG＋3 分鐘逾時、LED 燈號、flash 儲存，見第 2.2 節）。
- 熱點設定模式（AP_CONFIG）：手機連上熱點自動跳出設定頁（captive portal），可掃描附近 WiFi、填個案資訊，會帶入既有設定值；KEY0 按住取消退出（不儲存）。
- BLE 接收四種讀值（額溫槍、血氧計、血壓計、血糖，同一台 D40 二合一機的血壓/血糖）並正確上傳，見第 6 節協定細節。血糖數值已跟裝置螢幕比對一致，見第 7.2 節第 11 點。
- WiFi 連線 + HTTPS 上傳，含失敗自動重試（佇列持久化在 flash，斷電不遺失）、NTP 校時、KEY1 強制重新校時。
- 電子紙顯示器（Waveshare Pico-ePaper-2.9）Phase 1+2：四種模式的畫面都燒錄驗證過，見第 12 節。
- 板載按鍵 KEY0（長按進 AP_CONFIG／按住取消）、KEY1（手動觸發完整 WiFi 動作+強制 NTP 校時）、KEY2（未上傳紀錄畫面，可翻頁，2026-08-28 改版，見第 12.8 節）（2026-08-06 燒錄後實機驗證的是改版前的已上傳歷史版本，見第 2.2 節）。
- 每台裝置唯一衍生的 AP 熱點 SSID（2026-08-06 實機確認畫面顯示的 SSID 跟手機實際掃到的一致；密碼後來改成所有裝置固定同一組，見第 8 節第 39 點）。

**已寫完程式碼、編譯過關，但還沒燒錄/實機測試：**

- 血壓計時鐘合理性檢查（跟 NTP 比對，不合理就退回用 Pico 收到時間，容許誤差 7 天**待確認**，見第 7.3 節）。
- 上傳伺服器網址／認證金鑰改成可透過 AP_CONFIG 設定、TLS 憑證驗證框架（等正式後端網址才能真的填憑證/收緊驗證）。
- 已上傳紀錄保留機制（主持人要求，保留最近 200 筆，數字**待確認**，見第 5/7.3 節）——保留筆數上限/機制本身還沒有長時間運作驗證過；**2026-08-28 起 KEY2 畫面預設改成顯示未上傳紀錄**（見第 12.8 節），已上傳歷史目前沒有專屬畫面可以看，只留在 flash 裡（`storage_get_upload_history()` 等函式還在，但沒有呼叫端）。
- Flash 持久化改用 littlefs，解決 wear-leveling 問題（一次性、不相容的格式改動，見第 5.1/8.5 節第 28 點）——這次燒錄測試期間看起來運作正常（設定/待傳/歷史都有正確存取），但還沒驗證重開機後資料是否正確持久化。
- 修復無螢幕版本（不接電子紙板）開機後極可能卡死的問題（見第 12.9/8 節第 33 點）——這次測試機器有接面板，還沒拿無螢幕機器驗證過。
- NTP 一直沒校時成功時每 5 分鐘自動重試（見第 2.4 節）——這次測試很快就校時成功，沒機會驗證這個計時器本身。
- 2026-08-13：電子紙 BLE_RECEIVE 畫面版面優化（ID/狀態文字合併一行、Last upload/Pending 合併一行，省下來的空間留給以後 MD6 用，見第 12.6 節）——已 build 成功，還沒燒錄/肉眼驗證實際排版。

**協定研究完成，但完全還沒寫程式碼（下次接續開發先看這裡）：**

- FORA MD6 六合一測試儀藍牙協定：已反編譯官方程式確認大致格式，但機型辨識方式（ProjectNo/廣播名稱）、六個新項目的數值單位/scale 都還不確定，**故意還沒動 `fora_device_kind_t`／解析邏輯**，避免拿不確定的假設寫出連線階段就分類錯誤的程式碼。完整發現記錄在第 6.5 節，下次要接續前**先看完第 6.5 節最後的「下次接續開發」清單**。

**曾經做過又移除的：** 電子紙 Phase 3 局部刷新——重新檢視後發現沒有實測觀察到的具體場景真的需要它，換不到的好處不值得承擔未測試的風險，決定拿掉、維持只用全刷（見第 12.5 節）。

**還沒做完/還沒驗證，見第 7 節完整清單（含第 7.3 節「待與相關人員確認事項」——這幾項需要外部決策，不是我能自己判斷定案的）：**

- 24 小時等級耐用性測試完全沒做過——這是這次交接的主要任務。
- 重開機後 littlefs 資料持久化、無螢幕版本相容性、NTP 5 分鐘自動重試計時器、血壓計時鐘合理性檢查都還沒做，見第 7.2 節。

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

**（2026-08-28 改版）** 開機不再需要按任何按鍵：`state_machine_run()` 每次通電一律先進 `STATE_AP_CONFIG`（開熱點+設定頁），給 `AP_CONFIG_BOOT_TIMEOUT_MS`（3 分鐘，`state_machine.c`）的時間上限，逾時都沒人送出表單/按 KEY0 取消就自動放行到 BLE_RECEIVE。原本輪詢 BOOTSEL 決定要不要進 AP_CONFIG 的 `mode_boot_select.c/.h`（技巧：暫時切換 QSPI_SS 腳位讀電位，需要在 BLE/WiFi/core1 都還沒啟動前執行，正常運作期間執行有當機風險，見下方歷史記錄）已整個移除，`CMakeLists.txt` 也拿掉了這個編譯項目。

**改版動機**：field 部署時要求「插電就能立刻掃碼設定」，不希望還要教使用者「開機瞬間按住 BOOTSEL」這個對非工程背景的人來說不直覺的手勢。**已知取捨**：這代表**每一次斷電重開機**（包含非預期的電源不穩/重開）現在都會先卡在 AP_CONFIG 最多 3 分鐘才恢復 BLE 監測，比改版前「預設直接進監測模式」多了一段固定的監測空窗；3 分鐘這個數字是配合這次需求給的預設值，不是使用者逐項確認過的精算值，如果實測發現空窗期造成困擾（例如部署現場電源不穩、常常重開機），可以調小 `AP_CONFIG_BOOT_TIMEOUT_MS` 或者改回需要動作才進入。

**KEY0 長按 3 秒**（`mode_ble_receive.c` 主迴圈期間，見下方）**仍然是進入 AP_CONFIG 的手動路徑**，不受這次改版影響，讓使用者不用等下次斷電重開機就能隨時重新設定；這條路徑進去**不會**套用 3 分鐘逾時（`mode_ap_config_run(0)`），沿用改版前「等到送出表單或按 KEY0 取消為止」的行為，因為這是使用者主動要求設定，不應該被時間打斷。

**（2026-08-06 加做，尚未實機測試）** 手上實際裝的電子紙是 Waveshare **Pico-CapTouch-ePaper-2.9**（觸控版，見第 12.1 節更正），板上帶 KEY0/KEY1/KEY2 三顆按鍵（不是 4 顆，也不是觸控本身，另一顆是接硬體 RESET 的 RUN 鍵）。加了新的按鍵觸發點（`src/button_input.c/.h`）：
- **KEY0 長按 3 秒**：在 BLE_RECEIVE 模式期間隨時可以觸發，不用重開機，直接切去 AP_CONFIG（見 `mode_ble_receive_run()` 的 `MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG`）。**在 AP_CONFIG 畫面裡改成短按住 1 秒**（`AP_CONFIG_CANCEL_HOLD_MS`，跟進入用的 3 秒是同一個實體按鍵、同一個 `button_input_key0_long_press()` 函式、分別傳不同的 `hold_ms`）：不想改設定的話不用真的把網頁表單送出來，按住 KEY0 一下就能取消、退回 BLE_RECEIVE、不會儲存。取消的門檻刻意比進入低但不是瞬間單擊——連著的手機可能正在填表單，瞬間誤觸會讓熱點斷線、表單內容全部消失，需要一點保護但又不用像進入那麼久。
- **KEY1 按一下**：不管待傳佇列是否為空都會觸發完整一次 WiFi 動作（連線→強制重新 NTP 校時→上傳，見 `mode_upload.c`），跳過 idle timeout 等待——就算沒有資料要傳，使用者也可能只是想手動確認一次網路時間校得準不準。

**因為可能會有沒有接電子紙的「無螢幕版本」，這幾個按鍵的 GPIO（GP2/GP3/GP15）用軟體設內建上拉（pull-up）讀取，沒有實體按鍵接上去的機器永遠讀到「沒按下」，不會誤觸發，不需要用編譯選項區分兩種硬體、也不影響任何現有機制。**

### 2.3 板載 LED

Pico W 的 LED 接在 CYW43 晶片的 GPIO0，只能透過 `cyw43_arch_gpio_put()` 控制，且必須在主協作式迴圈中呼叫（不可在中斷處理常式中呼叫）。

### 2.4 NTP 校時重試頻率（2026-08-06 加做，尚未實機測試）

原本只有 `mode_upload_run()` 被觸發時才會嘗試 NTP 校時，而觸發 UPLOAD 的條件是「收到裝置讀值後才開始倒數 idle timeout」——**如果裝置一直沒有收到任何 BLE 讀值，永遠不會主動切去 UPLOAD，也就永遠沒有機會嘗試校時**。修法：`mode_ble_receive.c` 主迴圈新增一個獨立計時器，不依賴有沒有收到過裝置讀值，只要 `!wall_clock_is_synced()` 且距離上次嘗試超過 **5 分鐘**（`NTP_UNSYNCED_RETRY_MS`，使用者確認的值）就主動觸發一次 UPLOAD 嘗試校時，跟 KEY1 走同一條路徑（`mode_upload.c` 已經改成不管有沒有資料要傳都會嘗試 `wall_clock_sync()`，見第 8 節第 37 點）。校時成功後這個計時器就不會再觸發。

**刻意的設計選擇**：失敗幾次都不會拉長重試間隔（不做 exponential backoff），固定每 5 分鐘重試一次，直到成功為止——使用者確認過這個簡單版本可以接受，即使環境真的連不上網路，也會一直固定頻率重試、持續打斷 BLE 掃描一小段時間，這是接受的代價。

**2026-09-01 修正（跟上面「接受的代價」牴觸，改成真的有退避）**：實際使用發現「WiFi 密碼/SSID 設錯」這種永遠連不上的情況下，固定 5 分鐘重試、每次最壞要試 5 種認證模式（`mode_upload.c` 的 `WIFI_AUTH_MODES_TO_TRY`）才放棄，使用者觀感上像是「一直卡在 WiFi 連線失敗畫面」。改法：
- `WIFI_CONNECT_TIMEOUT_MS`（每種認證模式的逾時）從 30 秒降到 12 秒，5 種模式最壞情況從 150 秒降到 60 秒內就會放棄——只影響「注定連不上」的失敗路徑多久才死心，合法網路通常第一個對的認證模式幾秒內就連上，不受影響。
- `WIFI_FAILURE_BACKOFF_MS`（連線失敗後的退避冷卻期）從 5 分鐘拉長到約 1 小時。`mode_upload_in_backoff()` 這個既有函式（`mode_ble_receive.c` 的 NTP 重試、idle timeout 觸發都會檢查）本來就會擋下冷卻期內的自動觸發，只是原本設 5 分鐘等於沒什麼效果；拉長到 1 小時之後，NTP 重試的計時器雖然還是每 5 分鐘到期，但大部分時間會被這個檢查擋下，實際重試間隔變成約 1 小時一次。KEY1 手動觸發不受這個限制，想立刻重試隨時可以按中鍵。

## 3. 狀態機設計

```
開機
  │
  ▼
[AP_CONFIG]   熱點設定模式，LED 常亮，畫面顯示目前已存的設定（沒改過就是舊值）
  （2026-08-28 改版）每次通電一律先進這個狀態，不再需要按 BOOTSEL；給 3 分鐘
  （AP_CONFIG_BOOT_TIMEOUT_MS）時間上限
  設定完成並儲存（網頁表單送出）→ [BLE_RECEIVE]
  KEY0 按住 1 秒（不想改，直接離開，不儲存）→ [BLE_RECEIVE]
  逾時 3 分鐘都沒送出/沒取消 → [BLE_RECEIVE]（跟 KEY0 取消一樣不儲存）

[BLE_RECEIVE] 藍芽接收模式（預設常駐狀態）
  掃描/重連中 → LED 慢閃
  已連線、正常接收 → LED 心跳短閃
  收到任一裝置的資料 → 寫入 storage（同時落地 flash，經過判重，見第 6 節）
  收到第一筆資料之後，開始倒數：距離「最後一筆資料」超過設定的秒數沒有新資料
    且待傳佇列非空 → [UPLOAD]
  （在收到任何資料之前不會倒數；判重把資料濾掉、佇列仍是空的話也不會切換，
   避免血壓計那種要 30~45 秒才會推播一次的裝置被提早打斷，也避免白跑一趟
   WiFi 只為了確認「沒有東西要傳」）
  KEY0 長按 3 秒 → [AP_CONFIG]（見第 2.2 節，隨時可觸發，不用重開機；這條路徑
    進去不套用 3 分鐘逾時，等到送出表單或 KEY0 取消為止）
  KEY1 按一下 → [UPLOAD]（不管待傳佇列是否為空都觸發，跳過 idle timeout 等待，
    是完整的一次 WiFi 連線+強制重新 NTP 校時+上傳動作）
  還沒校時成功、且距離上次嘗試超過 5 分鐘 → [UPLOAD]（不用等收到裝置讀值，
    見第 2.4 節「NTP 校時重試頻率」）

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
| 血氧計 | **非固定值，見下方說明** | **未知，待確認**（見第 7 節待確認事項） | 表格初始值是 0，實際冷卻時間由量測邏輯動態覆寫 |
| 血壓計 | 200 秒（約 3 分 20 秒） | 3 分鐘 | 2026-08-26 使用者要求從原本多留 1 分鐘餘裕（240 秒）改成只多留 20 秒，換取連續量兩次時能更快收到第二筆 |
| FORA MD6 六合一 | 10 秒 | 未知（多合一機型沒有單一休眠門檻可參考） | 刻意設短，方便使用者連續測不同試片；每次連線結束前還會做一段「往回翻頁」抓同一次測試 session 其餘項目，見第 6.5 節 |

**2026-08-26 校正（跟程式碼核對後更新，`mode_ble_receive.c`）**：血氧計早已不是簡單的固定冷卻時間，`DEVICE_RECONNECT_COOLDOWN_MS[FORA_DEVICE_OXIMETER]` 只是開機後、還沒開始一輪觀察 session 之前用的初始值 0，實際邏輯改成「30 秒觀察視窗」：血氧計是夾著手指持續量測的裝置，連線後每 3 秒（`OXIMETER_RESAMPLE_INTERVAL_MS`）重新取樣一次候選值，視窗內只保留最新一筆，滿 30 秒（`OXIMETER_SETTLE_WINDOW_MS`，比照臨床「等 30-60 秒讀數穩定再記錄」的慣例取下限）才真正送出當作正式讀值；送出之後才進入 90 秒（`OXIMETER_POST_SETTLE_COOLDOWN_MS`）的冷卻，避免緊接著又開始下一輪觀察。

## 4. LED 燈號規範

| 狀態 | 燈號 | 時序 |
|---|---|---|
| 熱點設定模式（開機自動進入或 KEY0 觸發） | 常亮 | — |
| BLE 接收－掃描/重連中 | 慢閃 | 1000ms on / 1000ms off |
| BLE 接收－已連線、正常接收 | 心跳短閃 | 每 2000ms 閃 50ms |
| 上傳模式 | 快閃 | 150ms on / 150ms off |
| 錯誤/異常 | 三連閃 + 停頓 | 3×(80ms on/off) 後停 1200ms，循環 |

## 5. 資料模型

- `device_config_t`：WiFi SSID / 密碼、個案姓名、個案編號、個管師資訊、上傳伺服器網址、上傳認證金鑰（見第 8.5 節）。
- `vital_type_t`：`UNKNOWN`(0) / `TEMPERATURE`(1) / `SPO2`(2) / `PULSE_RATE`(3) / `SYSTOLIC`(4) / `DIASTOLIC`(5) / `GLUCOSE`(6，2026-08-06 已實機驗證解析/判重/上傳全部正確、且跟裝置螢幕數字比對一致，見 6.4/7.2 節第 11 點)。
- `vital_record_t`：`received_at_ms`（Pico 收到時間，或裝置有自己的量測時間戳時會被那個值取代）、`vital_type_t`、數值、上傳時間、上傳狀態，以及：
  - `device_measured_key`：裝置自己認證過的量測時間戳（分鐘解析度，`fora_protocol.c` 編碼/解碼），0 代表這種裝置沒有這個資訊。判重優先用這個值，見第 6 節。
  - `source_kind`：是哪種裝置回報的（`fora_device_kind_t`，用不透明的 `uint8_t` 存，避免 `common.h` 依賴 `fora_protocol.h`）。用途：避免共用同一個 `vital_type_t` 的不同裝置（例如脈搏同時來自血氧計跟血壓計）互相污染彼此的判重結果，見第 6 節。
- `LOCAL_UTC_OFFSET_SEC`（`common.h`）：專案目前只在台灣用，統一假設本地時間是 UTC+8，`display_status.c`（NTP 校時換算）跟 `fora_protocol.c`（裝置自己時鐘的量測時間換算）共用同一份常數。

**已上傳紀錄保留機制（2026-08-06 加做，尚未實機測試，主持人明確提出的需求）**：原本 `storage_mark_uploaded()` 上傳成功的紀錄會直接從待傳佇列移除、不再保留在任何地方。**主持人不希望上傳完後本機資料被直接清除，要求至少保留紀錄**。修法：新增一個獨立的環狀緩衝（`storage.c` 的 `s_upload_history[]`／`storage_get_upload_history()`），保留**最近 200 筆**已上傳成功的紀錄，持久化在 littlefs 分區裡的 `history.bin`（見下方 5.1 節），滿了就覆蓋最舊的一筆，不會無限成長。**200 這個數字是概略估計，不是主持人指定的精確值**：抓的假設是單一個案就算四種生理值都密集量測（一天合計約 20 筆讀值），200 筆大約涵蓋 1~2 週份量，在「保留多少歷史」跟「flash 空間/磨損」之間找一個折衷——這個假設本身沒有實際使用數據驗證過，如果之後發現實際量測頻率跟這裡的估計差很多，應該回來調整。目前**還沒有任何介面可以讀出這份歷史**（沒有畫面顯示、沒有上傳/匯出機制），只是先把資料保留下來，之後如果需要查驗可以再加讀取介面（例如序列埠指令、或另開一個除錯用的 HTTP endpoint）。

### 5.1 Flash 持久化改用 littlefs（2026-08-06 加做，尚未實機測試）

原本 `storage.c` 直接手刻三個固定 flash sector（config／待傳佇列／已上傳歷史）各自存一份資料，**沒有 wear-leveling**：同一個 sector 每次新增/上傳都整份覆寫，抹寫次數會一直集中在同幾個 block 上。改成掛載 [littlefs](https://github.com/littlefs-project/littlefs)（v2.11.3，vendored 在 `littlefs/`，BSD-3-Clause），三份資料變成同一個 littlefs 分區裡的三個檔案（`config.bin`／`pending.bin`／`history.bin`），抹寫會在分區內的多個 block 之間輪替，其餘 RAM 邏輯（判重、環狀緩衝索引、陣列壓縮）不變。

- **分區大小**：flash 最後 256KB（`LFS_PICO_PARTITION_SIZE`，`src/lfs_pico_hal.h`）。目前三份資料實際加總約 20KB，256KB 留了充裕的餘裕給 wear-leveling 輪替；程式碼本身（`text` 段）約 826KB／2MB，扣掉這個分區後 flash 還有約 1.2MB（~59%）空間可以給未來的更新使用。
- **block 對應**：一個 littlefs block = 一個實體 flash sector（4096 bytes，`FLASH_SECTOR_SIZE`）；prog/cache 對齊 `flash_range_program()` 要求的 256-byte 邊界（`FLASH_PAGE_SIZE`）；`block_cycles = 500`（littlefs 官方建議範圍 100-1000 的中間值）。實作見 `src/lfs_pico_hal.c`：讀取走 XIP 位址直接 `memcpy`，寫入/抹除透過 `flash_safe_execute()` 包住 `flash_range_program`/`flash_range_erase`（RP2040 抹寫 flash 期間 XIP 無法使用）。全部用靜態緩衝區，不使用 littlefs 內建的 `lfs_malloc()`。
- **一次性、不相容的格式改動**：舊版三個固定 sector 存的原始資料，littlefs 看不懂，`lfs_mount()` 第一次掛載會失敗並自動 `lfs_format()`——**這代表升級這個版本會清空 Pico 上原本存的設定/待傳資料/已上傳歷史，不會嘗試搬移或轉換舊資料**。這是刻意接受的代價，因為現階段還在開發測試、沒有正式量產資料需要保留；如果之後要對已經在現場運作的裝置做這個升級，需要另外考慮遷移方案。
- **還沒驗證**：只確認過完整重新編譯成功（383/383 targets），還沒有實機燒錄測試過——特別是 config/待傳/歷史三份資料能不能正確持久化並在重開機後讀回、第一次掛載自動格式化的行為是否符合預期。

## 6. 支援的裝置與藍牙協定（重要，之後擴充新裝置前必讀）

三種裝置都用裝置廣播名稱含 `"FORA"` 判斷是不是要連線的裝置，再用名稱裡的其他字元判斷是哪一種型號（見 `fora_protocol_matches_advertisement()`）。**2026-08-26 已改成每一種都要求明確比對到型號關鍵字**（不再是「其餘都當成額溫槍」的舊邏輯）：名稱含 `"FORA"` 但比對不出 `"O2"`／`"D40"`／`"IR42"` 任何一個關鍵字的裝置會直接回傳 false、不連線——這是為了修一個實際發生過的事故：FORA MD6（還沒支援）曾經被舊邏輯誤判成額溫槍，回應被誤解成一筆體溫讀值（錯誤數值如 12.8°C）上傳出去。

**實機確認過的廣播名稱**（2026-08-26，用 Arduino IDE 序列埠監控直接看 `debug_print_advertisement()` 的 log confirm，見 `mode_ble_receive.c`）：
- 額溫槍：`"FORA IR42"`（先前只是猜測，這次終於實機驗證過，猜對了）
- 血氧計：`"FORA O2"`（先前已確認）
- 血壓計：`"FORA D40"`（先前已確認）
- FORA MD6 六合一：`"FORA MD6"`——含明確的 `"MD6"` 字串，可以直接明確比對，不會跟其他型號混淆。

### 6.1 FORA IR42 額溫槍（`FORA_DEVICE_THERMOMETER`，kind=1）

- 判斷依據：名稱含 `"FORA"`，且含 `"IR42"`（2026-08-26 前是「不含 O2/D40 就預設當這一種」，已改成明確比對，見上方說明）。
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

### 6.4 FORA D40 血糖計部分（協定已反推確認並實作，2026-08-06 已實機驗證解析正確，見第 7.2 節第 11 點）

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
- **已實作、已實機驗證（2026-08-06）**：`fora_protocol_parse_reading(FORA_DEVICE_BLOOD_PRESSURE, ...)` 收到組好的 8 bytes 後會先檢查 `byte[2] & 0x80`，0 就照上表解析血糖、回傳 `VITAL_TYPE_GLUCOSE`，非 0 才照原本的血壓邏輯解析；`mode_ble_receive.c` 原本「送 part A/B、組 8 bytes」的流程不用改，分流是在解析階段做的。實機測試量到一筆血糖 107 mg/dL，`byte[2]` bit7 正確判斷成血糖、數值換算跟手動核算封包位元組吻合，完整走過解析→判重→存 flash→上傳全部成功。畫面（`display_status.c` 的血糖列）、上傳（`upload_api.c`／`test_server/app.py` 的 `VITAL_TYPE_NAMES`）也都正確顯示/傳送。**已跟裝置螢幕實際顯示的數字比對一致**，見第 7.2 節第 11 點。

### 6.5 FORA MD6 六合一測試儀（2026-08-26 已實機驗證：連線/配對/正式解析/多筆記錄往回翻頁全部跑通，見下方「目前還沒解決的問題」）

**現況一句話**：MD6 走跟 D40 完全同一套指令管道，只是把血糖泛化成六個項目；廣播名稱、連線/配對、兩段式取記錄、正式解析（`fora_protocol_parse_reading()` 已經是正式路徑，會存進 storage/上傳，不再是診斷輸出）、`cmd 0x2B` 問記錄筆數、非 0 index 往回翻頁抓同一次測試的其他項目，全部都已經實機驗證成功。目前還沒解決的是「Hb 好像不是獨立記錄」「HCT 以外的其他項目數值 scale 沒驗證過」「往回翻頁沒有 flash 持久化基準點，每次連線都整批重抓」，見下方清單。

**反推來源跟怎麼重現**：這台機器（`WunKong` 帳號）上裝了官方 Windows 程式「FORA Health Care Management System_BLE」（`%APPDATA%\FORA Health Care Management System_BLE\`，`image\` 資料夾裡有一張「福爾旗艦6合1測試儀 FORA MD6.jpg」確認這支程式認得 MD6），反編譯目標是同一支 `DLL\BLE_PCLink_Library.dll`，工具鏈跟第 8 節記錄的血壓計反推方法完全相同（`ilspycmd` 8.2.0.7535）。**這台機器上 `dotnet`／`ilspycmd` 都已經裝好，但不在這個 session 的 PATH 裡**——跟 pico-sdk 工具鏈一樣的問題（見第 0 節），要重新反編譯的話：

```powershell
$env:Path = "C:\Program Files\dotnet;$env:USERPROFILE\.dotnet\tools;" + $env:Path
ilspycmd -p -o <輸出資料夾> "$env:APPDATA\FORA Health Care Management System_BLE\DLL\BLE_PCLink_Library.dll"
```

反編譯出來的原始碼裡完全沒有出現過 "MD6" 這個字面字串（連 exe 的字串資源也搜不到），下面的內容是從命名空間裡看到的 class 結構類推出來的，不是直接看到寫死的 "MD6"。

**已確認的部分（有反編譯原始碼逐行對照）**：

- 對應 `TaiDoc.BLE_PcLink.Meter` 命名空間的 `GenBgmMeter` class（`MeterType.BG`）+ `TaiDoc.BLE_PcLink.Meter.Record` 命名空間的 `BloodGlucose` class——這個 class 其實是「多合一血糖機」的通用實作，D40 的血糖功能（第 6.4 節）已經在用同一套邏輯了，MD6 只是同一個 class 支援的其中一種型號。
- 指令框架、`FORA_BP_CMD_GET_RECORD_PART_A/B/COUNT`（`0x25`/`0x26`/`0x2B`）、8-byte checksum 全部沿用第 6.3/6.4 節已經實作的邏輯，不用新增指令。
- `byte[0..3]` 日期/時間解碼方式跟血壓/血糖完全一樣（見第 6.4 節表格），不用改。
- `byte[4..5]`：16-bit 小端數值，`65535` 視為無效讀值（跟血糖一樣）。
- `byte[6]`：ambient（環境溫度），不用。
- `byte[7]` 是 MD6 新增的關鍵欄位，跟血糖共用同一個 byte 但語意更豐富：
  - bits 6-7（`&0xC0`，右移 6）＝量測情境：0=一般（`byte[3]` bits5-7==4 時視為「運動」）、1=飯前(AC)、2=飯後(PC)、3=QC（品管/對照液測試）。**2026-08-26 實機驗證發現這個 QC 慣例只在血糖項目上成立**：往回翻頁抓到一筆 HCT=50（跟裝置螢幕顯示值相符，不是品管測試），但它的 context bits 剛好是 3——如果比照血糖邏輯把 context==3 一律當 QC 丟掉，會誤刪這種真的病人數值。目前 `fora_protocol.c` 已經改成**只有 `item_code==0`（血糖）才套用這個過濾**，其餘 5 項不套用；這 2 個 bit 對非血糖項目實際代表什麼還不知道。
  - bits 2-5（`&0x3C`，右移 2）＝這筆記錄是哪一項，代碼共用自「多合一血糖機」通用協定 class（見上方說明），但 **MD6 是「六合一」機型，實際只會出現其中 6 個**：`0`=血糖 Glucose、`6`=HCT（血球比容/紅血球容積比）、`7`=酮體 Ketone、`8`=尿酸 UA、`9`=總膽固醇 CHOL、`11`=血紅素 HB。`12`=乳酸 Lactate、`13`=三酸甘油脂 TG 是同一套協定 class 給其他型號（例如八合一之類的機型）用的代碼，MD6 不會回報這兩個（2026-08-26 使用者確認：MD6 官方就是六合一，不是八合一，先前這裡誤寫成 8 項）。
  - bits 0-1：跟項目欄位共用「code number」概念（不同項目用不同試片），這個專案目前不需要額外解析。

**~~2. 廣播階段沒辦法用裝置名稱分辨是不是 MD6~~ 已解決（2026-08-26）**：原本以為要連線送 `cmd 0x24` 才能分辨，結果實機用 Arduino IDE 序列埠監控直接看 `debug_print_advertisement()` 的 log，MD6 的廣播名稱其實就是 `"FORA MD6"`，明確含 `"MD6"` 字串，跟 O2/D40/IR42 完全不會混淆，廣播階段就能直接明確分辨，不需要「連線後送 cmd 0x24 查 ProjectNo」這個工作量比較大的方案。目前 `fora_protocol_matches_advertisement()` 已經改成「每種型號都要求明確比對關鍵字」（見第 6 節開頭的說明），MD6 目前會正確地「認出名稱含 FORA、但比對不出任何已知型號」而回傳 false、不連線——這個安全行為已經實機驗證過（MD6 廣播了 13 次都沒有被誤連線）。**下一步只要把 `"MD6"` 加進去比對清單、新增 `FORA_DEVICE_MD6` kind 就能開始連線階段的開發**，不用再繞去查 ProjectNo。

**~~廣播/連線/配對/兩段式取記錄/正式解析~~ 已實機驗證成功（2026-08-26）**：`fora_protocol.h` 已加 `FORA_DEVICE_MD6`，`fora_protocol_matches_advertisement()` 已加 `"MD6"` 比對，`fora_protocol_parse_reading()` 的 MD6 分支已經是正式解析路徑（不是診斷輸出），`common.h` 已加 `VITAL_TYPE_HCT/KETONE/UA/CHOL/HB`，`upload_api.c` 已加對應 JSON 欄位跟 `dataSource=FORAMD6`。連線流程完全比照血壓計（需要配對，`sm_request_pairing()`），送 `cmd 0x25`/`0x26` 兩段式取記錄跟 D40 共用同一套邏輯。血糖數值驗證：裝置螢幕 128 跟解出來的 raw_value 一模一樣，不需要縮放，`byte[4..5]` 16-bit 小端直接就是 mg/dL。日期時間解碼也驗證正確（跟裝置實際量測時間一致，例如 2026-08-26 10:03）。

**~~多筆記錄往回翻頁~~ 已實機驗證成功並實作（2026-08-26）**：`mode_ble_receive.c` 的 `md6_backfill_state_t` 狀態機——index=0 這筆正式 commit 之後，連線先不斷線，送 `cmd 0x2B` 問記錄筆數（`byte[2]|byte[3]<<8`，實測回應 4/6 都跟實際能抓到的筆數吻合），接著把 `send_bp_get_record_part_at_index()` 的 index 塞進 p1/p2（16-bit 小端），從 index=1 一路往回抓到裝置回報的筆數（安全上限 `MD6_BACKFILL_SAFETY_CAP=8`，連續 2 筆解析失敗也會停手）。同一次連線內完整實測過一次「3 合一試紙測 2 次 + 裝置裡一筆 5 月舊記錄」共 6 筆，全部正確抓到、正確歸類、正確存進待傳佇列，一筆沒漏（過程中抓到並修正了兩個實作 bug，見下方）。

**已解決的實作 bug（2026-08-26，都是靠實機 log 抓到的，不是 code review 發現的）**：

1. **QC 過濾要不要套用在非血糖項目，來回改了三次，最後用實機螢幕畫面定案：只在血糖套用**。時序：
   (1) 往回翻頁抓到一筆 HCT=50（跟裝置螢幕相符）卻剛好情境 bits==3，一開始臆測「數字看起來合理，不像 QC」，改成只在血糖套用過濾；
   (2) 反編譯官方 PC 端程式（`TaiDoc.BLE_PcLink.Meter.Record.BloodGlucose` class）發現官方碼對全部項目統一套用「情境 bits==3＝QC」，改成統一套用——但這個推論其實有漏洞：反編譯出來的只是「PC 軟體怎麼分類/標示」，不代表官方軟體看到 QC 分類就會把記錄丟棄不存，這一步的邏輯跳躍沒有根據；
   (3) 使用者直接核對裝置螢幕：那 2 筆 HCT（46、50）**螢幕上完全沒有顯示 QC 標記**，確認是正常讀值，改回只在血糖套用——**這次有實機畫面證據**。使用者後續補充的官方說明書內容也解釋了原因：裝置對「血糖／3合一／酮體試片」有自動 QC 偵測（吸入品管液才會觸發，操作者用真血液測試不會誤觸發），但**膽固醇／尿酸的 QC 要手動按 M 鍵才會啟用，不會自動偵測**——這暗示 context bits 對非血糖項目本來就不是可靠的「是否為 QC」指標，跟三次反覆的最終結論一致。`fora_protocol.c` 的 BP 血糖分支（D40）原本完全沒有這個過濾，這次也一併補上（同一套 `BloodGlucose` class，理論上有一樣的風險，D40 只測血糖，不受這次反覆影響）。詳見下方「反編譯出來的官方解析邏輯」跟「官方說明書：品管測試」。
2. **pending 佇列覆蓋邏輯只比對 type+source_kind，沒比對 device_measured_key，翻頁翻到舊記錄時會蓋掉剛抓到的新記錄**：`storage_append_record()` 原本的「同類型+同來源就直接覆蓋 pending」對血壓計/血糖機這種「只有一筆當前值」的裝置沒問題，但 MD6 一次連線會抓到同 type 好幾筆不同時間點的記錄，往回翻到比較舊的那筆時反而蓋掉剛抓到的新讀值，整批資料最後只剩最舊那筆能上傳。已修正：兩筆記錄都有 device_measured_key 時，要時間戳也相同才覆蓋，不同時間點的記錄各自獨立存成一筆 pending 記錄。~~額溫槍/血氧計（沒有裝置時間戳）行為不變。~~
   **2026-09-23 追加修正**：使用者實機反映額溫槍/血氧計離線期間連續量測多次，待傳佇列筆數卻不會增加——原因就是上面這句「行為不變」：這兩種裝置沒有 `device_measured_key`，走到覆蓋判斷時 `both_have_keys` 恆為 false，等同永遠符合覆蓋條件，導致同類型/同來源只要還沒上傳成功，新讀值一律蓋掉舊讀值，離線期間只有最後一次量測會真的進到待傳佇列。但這個情境其實已經被前面的 `is_duplicate` 判重擋過一輪了（數值相同且在 `DUPLICATE_SUPPRESS_WINDOW_MS` 視窗內才視為重複，見第 5 節 storage 模組說明）——走到覆蓋判斷這裡代表 `is_duplicate` 已經判定為 false，也就是數值不同或已超過視窗，幾乎可以確定是使用者真的又做了一次獨立量測，不該被覆蓋。修法：覆蓋規則收斂成「只有雙方都帶裝置時間戳、且時間戳相同才覆蓋」（`storage.c` 的 `both_have_keys` 判斷從 `if (both_have_keys && 時間戳不同) continue` 改成 `if (!both_have_keys || 時間戳不同) continue`），額溫槍/血氧計從此不再互相覆蓋，離線期間每一次不同的量測都各自新增成獨立的 pending 記錄。同時把 `DUPLICATE_SUPPRESS_WINDOW_MS` 從 10 分鐘縮短成 3 分鐘（使用者要求），讓「兩次量測間隔多久才不算重複」更貼近實際使用情境。已編譯驗證通過（RP2040 `build/` 與 Pico 2 W `build-pico2/` 皆過關）並燒錄進 Pico 2 W 實機，待使用者下一輪實機測試確認。
3. **量測情境（一般/飯前/飯後）已解析並上傳**：`common.h` 新增 `vital_record_t.measurement_mode`，`fora_protocol.c` 解析 `byte[7]` 高 2 bit 填入（血壓計血糖／MD6 都有），`upload_api.c` 依這個欄位把血糖分流到後端既有的 `glu_ac`（一般/飯前，預設）／`glu_pc`（飯後）兩個欄位，不再固定送 `glu_ac`。

**反編譯出來的官方解析邏輯（2026-08-26，`ilspycmd` 反編譯 `BLE_PCLink_Library.dll`，`TaiDoc.BLE_PcLink.Meter.Record.BloodGlucose` 建構子逐行核對）**：跟這個專案獨立反推出來的協定完全一致，包含日期/時間解碼、`value=byte[5]*256+byte[4]`、`ambient=byte[6]`。額外確認：`BloodParameter2` enum 就是 `Glucose=0, HCT=6, Ketone=7, UA=8, CHOL=9, HB=11, Lactate=12, TG=13`，跟這個專案的 item code switch 完全對應（證實 `HB=11` 這個映射本身沒有錯，不是猜的）。`codeNo = byte[7] & 0x3F`（這個專案目前不需要）。情境判斷 `num2 = (byte[7] & 0xC0) / 64`：0＝Gen（`byte[3]` bits5-7==4 時是「運動」子狀態，這個專案目前不解析）、1＝AC、2＝PC、3 或任何其他值＝QC。**這個判斷是 PC 軟體對全部項目統一套用的，但如上面第 1 點所述，這不代表 context bits 對非血糖項目也真的是「有沒有做 QC」的意思——實機證據跟官方說明書都指向並非如此。**

**官方說明書：品管測試（2026-08-26 使用者提供，`FORA MD6` 使用手冊節錄）**：
- 品管液測試結果應落在試片包裝標示的容許範圍內，用來檢視儀器/試片運作是否正常。
- **自動 QC 偵測只適用於血糖、3合1、酮體試片**：這幾種試片吸入品管液後，儀器會自動判定為 QC 模式，不須使用者手動切換。
- **膽固醇、尿酸的 QC 測試沒有自動偵測**：必須在試片插入後手動按 M 鍵選擇 QC 模式，否則「可能會導致錯誤的品管結果」（官方原文用詞）——換句話說，這兩項就算真的用品管液測試，如果使用者忘記按 M 鍵，裝置也不會自動標記成 QC。
- 品管測試結果會自動存進裝置記憶，並有「QC」標示。
- **裝置可儲存 1000 組含日期時間的測試結果**，超過 1000 筆時最新的會自動覆蓋最舊的（循環緩衝）。血糖有 7/14/21/28/60/90 天平均值功能；HCT/Hb/Ketone/CHOL/UA/QC 結果都不提供平均值計算——這點只是背景知識，跟這個專案的解析/上傳邏輯無關。**這個「1000 組」的容量數字對現有的往回翻頁安全上限有直接影響，見下方「目前還沒解決的問題」第 4 點。**

**2026-08-26 後續實機驗證（同一天，跟上面反編譯/QC 反覆同一輪）**：
- **D40 往回翻頁已實機驗證成功**：連線抓到 `record count=38`，安全上限 8 的情況下抓到 index=0~7 共 8 筆（1 筆血糖 GEN + 7 組血壓，各組 SYS/DIA/PULSE），全部正確解析、正確存成各自獨立的 pending 記錄，沒有互相覆蓋（storage.c 的修正對 D40 一樣有效）。
- **AC/PC 情境解析已實機驗證成功**：MD6 測到一筆飯後(PC)血糖 115（`byte[7]=0x81`），正確解析成 `mode=PC`，其餘同一批往回翻頁抓到的舊記錄都正確顯示 `mode=GEN`，兩者對比清楚。AC 因為跟 PC 是同一段程式碼、只有 bit 值不同（1 vs 2），使用者確認不需要另外測。血糖也已經依 `mode` 分流到 `glu_ac`/`glu_pc`（`upload_api.c`），但**上傳本身還沒實機驗證過**（卡在下面的 WiFi 密碼問題）。

**~~flash 持久化同步基準點 + 分批跨連線抓取~~ 已實作（2026-08-26），還沒實機驗證**：這是這一輪優先順序最高的項目——見上面「安全上限遠遠不夠」的說明，MD6 最多 1000 組、D40 實測至少 38 組，都遠超過單次連線能處理的量。設計／實作：
- `storage.h`/`storage.c` 新增 `backfill_sync_state_t{ synced_up_to_key, resume_index }`，依裝置種類（`source_kind`）分開持久化在新的 `backfill_sync.bin`（跟 `config.bin` 同樣的單一結構讀寫模式）。
- `mode_ble_receive.c` 的 `record_backfill_state_t` 翻頁流程改成：每次連線最多翻 `RECORD_BACKFILL_SAFETY_CAP`（8）個 index，起始 index 是 `resume_index`（沒在追進度時是 1，正在追一大批舊記錄時是上次沒抓完的地方）；翻頁途中如果碰到 `measured_key` 等於已知的 `synced_up_to_key`，代表追上了，立刻停止（不用每次都抓滿安全上限）；這次連線預算用完但還沒追上的話，記住 `resume_index` 存進 flash，下次連線接著翻；真的追上（或連續兩筆解析失敗，視為到底）之後，把 index=0 的 `measured_key` 存成新的 `synced_up_to_key`、`resume_index` 重置為 1，回到「穩態」（沒有新測試的話下次連線一翻頁就會立刻碰到新基準點，不會重問舊記錄）。
- **已知限制**：index 是「相對於目前最新一筆」的相對位置，追一大批舊記錄的過程中如果使用者又做了新測試，index 會漂移，`resume_index` 可能對不準——靠 `device_measured_key` 判重頂多是「效率打折/漏掉幾筆」，不會產生重複資料，但也不保證完全不漏。裝置沒有提供穩定的絕對記錄 ID，目前沒有更好的解法。
- **還沒實機驗證**：目前只在小量記錄（4~38 筆，一次連線內就能追完）的裝置上測過，沒有機會測到「一次連線追不完、要跨好幾次連線接力」的情況，這個分批續傳的路徑還沒有實機證據。

**~~Hb（血紅素）沒有獨立記錄~~ 已實作換算，2026-08-26 使用者決定要傳**：確認裝置往回翻頁抓到的記錄裡只有血糖跟 HCT 兩種 item code，從沒出現過 Hb 的 item code (11)，也沒有記錄解析失敗被跳過（不是漏抓，是裝置沒有這筆記錄）。核對公式 **Hb ≈ HCT × 0.34**（血液學常用換算係數）：2026-08-26 兩個實機樣本 46×0.34=15.64≈15.6、50×0.34=17.0＝17，**都精確吻合**裝置螢幕顯示的 Hb。已在 `fora_protocol.c` 的 MD6 分支實作：每次解析出一筆 HCT，額外用同一個 `measured_key`/`mode` 多回傳一筆用 `HCT×0.34` 算出來的 `VITAL_TYPE_HB`（`fora_protocol_parse_reading()` 現在 MD6 的 HCT 記錄回傳 2 筆，不是 1 筆）。**這是韌體自己估計出來的值，不是裝置量到的**，只用 2 個樣本驗證過換算係數，`upload_api.c` 的 `hb` 欄位註解已經標注這點。如果裝置以後真的回報 item_code==11 的真實 Hb 記錄，storage.c 的覆蓋邏輯（比對 measured_key）會自動用真實測量值蓋掉這裡的估計值。

**2026-08-26 使用者決定：拿掉 flash 持久化同步基準點（resume_index/synced_up_to_key），往回翻頁不設安全上限，單次連線內把裝置回報的筆數全部抓完**——原本設計的「flash 存進度、跨連線分批接力」完全沒有實機驗證過，而且是板子當機前最後加的一段程式碼（連線過程中會讀寫 flash），嫌疑最大；拿掉它可以同時解掉「這段完全沒測過」跟「跨連線 resume_index 可能因為使用者中途又測了新記錄而對不準」這兩個問題。改成單次連線內用既有已驗證過的「往回翻頁到裝置回報筆數」機制（`RECORD_BACKFILL_SAFETY_CAP`，MD6/D40 都實測過 walk index=1..N 沒問題），但把這個上限從 8 調高到遠超過官方文件寫的 1000 組上限（例如 1500），純粹當作防止 `cmd 0x2B` 回應格式解析錯誤/裝置回傳異常值時的無窮迴圈保險，不是拿來限制正常抓取——正常情況下都會被裝置實際回報的筆數（或連續兩筆解析失敗）提前結束，不會真的抓到那個上限。**這是規劃決定，還沒動手改程式碼**，下次要接續開發時：
1. 把 `mode_ble_receive.c` 現有（已經在改到一半)的 `record_backfill_state_t` 相關程式碼裡，所有 `backfill_sync_state_t`／`storage_get_backfill_sync_state()`／`storage_set_backfill_sync_state()` 的呼叫都拿掉，`finish_record_backfill()` 這個輔助函式也整個刪掉。
2. `handle_record_backfill_notification()` 的 `RECORD_BACKFILL_WAITING_COUNT` case 改回單純的「`target_count = min(count_guess, RECORD_BACKFILL_SAFETY_CAP)`，`next_index` 固定從 1 開始」，不要再讀 flash 基準點、不要有 `start_index`/`budget_end`/`known_synced_key` 這些概念。
3. `RECORD_BACKFILL_WAITING_PART_B` case 拿掉「碰到 `known_synced_key` 就提前結束」那段判斷（因為沒有基準點了），只保留「解析失敗連續 2 次」跟「`next_index >= target_count`」這兩個停手條件。
4. `RECORD_BACKFILL_SAFETY_CAP` 從 `8` 調成一個遠大於 1000 的數字（純安全網，例如 `1500`）。
5. `storage.h`/`storage.c` 的 `backfill_sync_state_t`／`storage_get_backfill_sync_state()`／`storage_set_backfill_sync_state()` 這幾個新增的東西如果沒有其他呼叫端在用，整個拿掉（含 `backfill_sync.bin` 那個 flash 檔案的讀寫）。
6. 改完要先確認 build 過關，燒錄前列出這次改了什麼，等使用者確認才燒。

**目前還沒解決的問題（使用者 2026-08-26 決定暫不處理）**：

1. **HCT 以外的 Ketone/UA/CHOL 數值 scale／單位還沒實機驗證過**（目前只有 Glucose、HCT 兩項有實測過裝置螢幕數字可以比對，都確認不需要縮放）。
2. **`cmd 0x2B` 回應格式（`byte[2]|byte[3]<<8`＝筆數）只驗證過三個樣本（4、6、10、38——D40 那次是 38），數字更大時是否還一致沒測過**。
3. **WiFi 密碼設錯，上傳全部卡在這一步**（`link status=1` 之後變 `-2`，全部認證模式都試過，`rc=-8`）：需要透過 AP_CONFIG 熱點設定模式重新輸入正確密碼，這步驟需要使用者在裝置上操作。在這個問題解決之前，AC/PC 分流到 `glu_ac`/`glu_pc`、Hb 估計值等等這些「解析階段」已經驗證好的邏輯，都還沒辦法驗證「上傳階段」真的會照預期送到後端。
6. **~~上傳批次一個 type 只會保留最後一筆值~~ 已修正（2026-08-26）**：`mode_upload.c` 原本只依 `source_kind` 分組、一組送一次上傳，往回翻頁抓到同一 type 好幾筆不同時間點的記錄時，`build_measurement_json()` 組 JSON 只會保留最後一筆，其餘被靜默丟掉。已改成先依 `source_kind` 分組，組內再依 `device_measured_key` 細分——額溫槍/血氧計沒有裝置時間戳（恆為 0），細分後行為不變（還是一組一次請求）；血壓計/MD6 一次連線抓到的好幾個不同時間點記錄，現在會各自送一次獨立的上傳請求。`storage_mark_uploaded_for_kind()` 也改名/改簽名為 `storage_mark_uploaded_for_group(source_kind, device_measured_key, ...)`，只標記剛剛那一組（裝置種類＋時間點）的結果。**還沒實機驗證過**（只驗證到 build 成功），下次配合 MD6 多筆記錄的情境一起測。

7. **~~`uploadTime` 一律是上傳當下時間，不是量測時間~~ 已修正（2026-08-31）**：上面第 6 點修好「同一個 type 好幾筆舊記錄各自送一次請求」之後，還留著一個問題——每一組送出去的 `uploadTime` 全部共用同一個「這次連線、發 HTTP 請求當下」的時間，不是那筆資料真正的量測時間。實際場景：個案出門吃飯量血糖，晚上回家中繼器才連上線，往回翻頁補抓到那筆血糖時，後端看到的 `uploadTime` 會是「晚上」，不是「量測當下（中午）」，跟螢幕上顯示的時間（D40/MD6 優先顯示裝置自己的量測時間，見第 12.6 節）不一致。已修正：新增 `fora_protocol_resolve_epoch_ms(received_at_boot_ms, device_measured_key)`（`fora_protocol.h`/`.c`），把 `display_status.c` 的 `format_reading_clock()` 原本的判斷邏輯（裝置有量測時間戳且合理就用裝置時間，否則退回中繼器收到時間）抽成共用函式；`mode_upload.c` 改成每一組各自呼叫這個函式算出自己的時間，不再是迴圈外算一次「現在」共用給所有組別。額溫槍/血氧計沒有裝置時間戳，行為不變（一律用中繼器收到時間）；D40/MD6 現在螢幕顯示跟後端 `uploadTime` 會是同一個時間來源。**已編譯成功（六個相關檔案），還沒有機會產出 .uf2 燒錄實機驗證**。

   **官方量測範圍規格（2026-08-26 使用者提供，用來驗證解析出來的數值合不合理**——不是拿來反推 scale，只是拿來檢查「解析出來的數字有沒有落在裝置本身宣稱的合理範圍內」）：
   - 血糖 Glucose：10-600 mg/dL（跟這次實測的 128 一致，範圍對得上）
   - 酮體 Ketone：0.1-8.0 mmol/L
   - 總膽固醇 CHOL：100-400 mg/dL
   - 尿酸 UA：3-20 mg/dL
   - HCT／HB 的官方量測範圍規格這次沒有拿到，需要之後補（Lactate／TG 不適用，MD6 是六合一機型不會回報這兩項）。

**原始設計（規劃階段記錄，跟上面「已解決」重疊的部分以上面實作為準；「flash 持久化基準點」這部分還沒做，設計仍然有效，見上方「目前還沒解決的問題」第 3 點）**：

- **同步演算法**：連線後從 index=0（最新）開始逐筆往回問。
  - **裝置第一次配對（沒有任何同步基準點）**：一路往回抓到裝置回「沒有更多記錄」為止，或抓到安全上限——把裝置裡現有的歷史記錄都拿回來，不是只拿最新一筆當起點。目前的實作沒有分「第一次/之後」，每次都當第一次做。
  - **之後每次連線（已有基準點，還沒實作）**：往回抓到「時間戳＋項目代碼都跟基準點一樣」的那一筆就停止。
  - **判斷「同一筆記錄」**：要用「時間戳（`device_measured_key`）＋ byte[7] 的項目代碼」兩者都相同才算，不能只比時間戳——同一次測試的不同項目（例如血糖+HCT）時間戳會一樣，只比時間戳會誤判成重複而漏掉（目前用 dedup 的 type+device_measured_key 隱含達到同樣效果，因為不同項目是不同 vital_type_t）。
  - **基準點要存進 flash**（不是只存 RAM），跟現有的 pending queue／history 一樣持久化，重開機不能忘記同步到哪——還沒實作。
  - **中途斷線是安全的**：下次連線重新從 index=0 開始問，已經抓過的記錄再問一次只會覆蓋同一筆 pending 記錄（不會變成重複資料），只是效率上多繞一輪，不影響正確性。已實機驗證（storage.c 的 dedup 邏輯，見上方「已解決的實作 bug」第 2 點）。
  - **~~血壓計（D40）沿用同一套機制~~ 已接上（2026-08-26），但還沒實機驗證**：`mode_ble_receive.c` 的 `record_backfill_state_t`（原本叫 `md6_backfill_state_t`，改成通用命名）觸發條件從「只有 MD6」改成「MD6 或 BLOOD_PRESSURE」，`handle_record_backfill_notification()` 內部呼叫 `fora_protocol_parse_reading()` 時也從寫死 `FORA_DEVICE_MD6` 改成用 `s_current_kind`，這樣血壓計往回翻頁抓到的記錄（可能是血壓 3 筆一組，也可能是血糖 1 筆，取決於裝置當下是哪一種模式，見 `fora_protocol.h` 的協定說明）都能正確解析。D40 的安全上限也是沿用 `RECORD_BACKFILL_SAFETY_CAP=8`，一樣不知道夠不夠（D40 的記錄容量沒有像 MD6 那樣查到官方數字）。
- **上傳範圍**：MD6 六合一全部項目（血糖/HCT/酮體/尿酸/總膽固醇/血紅素，**不含乳酸/三酸甘油脂，MD6 這台不會回報那兩項**）都上傳，韌體端不特別挑選哪些項目才傳，量到哪項就送哪項的 JSON 欄位——**已實作**。欄位名稱沿用裝置官方縮寫：`glu_ac`（已有）、`hct`、`ketone`、`ua`、`chol`、`hb`（這 5 個目前後端 `PhysioMeasurement` 沒有對應欄位，Gson 解析時會靜默忽略、不會報錯也不會存進資料庫，韌體這邊格式已經準備好，欄位的事還沒跟後端同步）。

### 6.6 Bionime Rightest GM700SB 血糖機（2026-08-28 已寫完程式碼、編譯過關，完全還沒實機測試——下次接續開發先看這裡）

**現況一句話**：跟廠商拿到正式協定文件（《GM700SB Data Communication Protocol - Rev 1.1》PDF，放在專案根目錄，標記 Confidential 故意不加入版控），照文件內容完整實作了連線/配對/PCL Mode 開關/型號核對/讀取記錄總數與書籤/逐筆讀記錄/解析成 `vital_record_t` 的整條路徑，`feature/rightest-gm700sb` 分支上已編譯過關（`cmake --build build`），**但完全沒有拿實機驗證過任何一步**，包含最基本的「連得上」都還沒測。

**跟 FORA 系列完全不同的地方**（第一次加入非 FORA 廠牌，見 `fora_protocol.h` `FORA_DEVICE_RIGHTEST_GM700SB` 開頭的說明）：
- 晶片是 Dialog Semiconductor DA1458x（LightBlue 讀 Device Information 服務確認），不是 FORA 系列共用的 Nordic nRF5 SDK 範例板，走完全不同的自訂 GATT service（`0xFEE0`，底下 `FEE1`=PCL Mode 開關、`FEE2`=Notify 回應通道、`FEE3`=Write 指令通道），不是 FORA 的 Nordic LED/Button Service（`00001523...`）。
- **廣播名稱是裝置序號**（協定文件明講，2026-08-28 LightBlue 實機確認過），沒有型號字串可以在掃描階段比對，`rightest_protocol_matches_advertisement()` 只能先用「廣播的 16-bit Service UUID 清單含 `0xFEE0`」粗篩，真正身份要連線配對、開了 PCL 之後送「查詢型號名稱」指令核對回應字串開頭是不是 `"GM7"`（見 `mode_ble_receive.c` 的 `RIGHTEST_SESSION_WAIT_MODEL_NAME` 處理）——**這代表可能會對到其他也用 `0xFEE0` 的陌生裝置先配對再確認身份失敗**，配對本身是有副作用的動作，2026-08-28 使用者已經知悉並接受這個取捨（沒有更早期、不需要配對就能核對身份的辦法）。
- **`FEE3`（指令通道）只支援 Write，沒有 Write Without Response**（2026-08-28 LightBlue 實機確認），跟 FORA 系列全部走 write-without-response 不一樣，`send_rightest_command()`/`send_rightest_pcl_mode()` 都改用會產生 `GATT_EVENT_QUERY_COMPLETE` 的 `gatt_client_write_value_of_characteristic()`——但這個 ATT 回應目前刻意不處理（見下面「時序假設」）。
- **回應可能跨多個 BLE Notify 封包**（協定文件的 GATT Payload Format：每個 payload 前面帶 2 bytes 表頭 `[總封包數][目前第幾包]`），FORA 系列每筆回應都在單一 Notify 收完，這次新增了 `rightest_protocol.c` 的 `rightest_reassembly_t`/`rightest_reassembly_feed()` 專門處理這個重組。
- **不需要自己在 flash 存「上次同步到哪」的定位點**：裝置自己維護一個「last transmission index」書籤，讀某個 index 的記錄會把書籤推進到那裡（協定文件明講），下次連線的 TYPE 1 查詢會直接告訴我們書籤在哪，從 +1 開始讀到目前總筆數就好——沒有比照 FORA MD6/D40 用 `storage_get_backfill_anchor()`/`storage_set_backfill_anchor()`。**這個「書籤是裝置端全域狀態、不是每個藍牙連線各自重置」的假設是從協定文件的範例推斷出來的，不是文件白紙黑字寫的，2026-08-28 完全沒有實機驗證過**，如果實測發現不成立（例如每次新連線書籤都被重置成 0），會導致每次連線都重新讀一遍全部歷史記錄——不會產生資料庫重複（`storage.c` 的 dedup 邏輯靠 `device_measured_key` 擋著），只是效率變差，不是正確性問題。

**架構決策**（2026-08-28 討論定案，跟純 FORA 裝置擴充不一樣的地方）：
- **裝置種類編號空間沿用 `fora_device_kind_t`**（新增 `FORA_DEVICE_RIGHTEST_GM700SB`），沒有另開一個獨立 enum——雖然這台根本不是 FORA 裝置，但 `mode_upload.c`（依 `source_kind` 分組上傳的迴圈）、`mode_ble_receive.c`（冷卻時間/handle 快取陣列）都是用 `[FORA_DEVICE_KIND_COUNT]` 固定大小的陣列/迴圈邊界，另開新 enum 會讓這些邊界對不上、需要大改好幾個檔案，語意上的瑕疵（名字是 FORA 但裝置不是）比這個風險更能接受。
- **協定/解析邏輯獨立成 `src/rightest_protocol.h`/`.c`**（不是塞進 `fora_protocol.c`），結構比照 `fora_protocol.c`：checksum、指令組裝、回應解析、廣播比對都在這裡，`mode_ble_receive.c` 只負責 BLE GATT 層的連線/探索/寫入/通知分派（跟 FORA 那幾種裝置的分工原則一致）。
- **`device_measured_key` 沿用 `fora_protocol_decode_measured_key()` 完全相同的 bit-packing 格式**（`(year-2000)<<20 | month<<16 | day<<11 | hour<<6 | minute`）——這樣 `display_status.c`/上傳邏輯既有的 `fora_protocol_measured_key_to_datetime()`/`_to_epoch_ms()` 可以直接沿用，不用為這台裝置另外寫一份日期換算，`display_status.c`/`upload_api.c` 完全不用改。
- **餐別標記簡化映射**（2026-08-28 使用者決定）：GM700SB 有 7 種（飯前/飯後/無餐/宵夜/睡前/運動/起床），簡化映射成既有 `fora_measurement_mode_t` 3 態——飯前→AC、飯後→PC，其餘都算 GEN，不新增欄位、不擴充後端，跟 FORA MD6/D40 的做法一致。
- **時區固定假設台灣**（2026-08-28 使用者決定）：每筆記錄自己帶一個 5-bit 時區欄位（協定文件範例算出 GMT-4，Meter TZ Index 4=UTC+8 對應台灣），刻意不解析，跟 FORA 一樣直接用 `LOCAL_UTC_OFFSET_SEC`。
- **沒有比照 `s_handle_cache` 做 GATT handle 快取**：GM700SB 不像 FORA 那樣量測完就急著斷線，每次連線都重新做完整的 service/characteristic 探索，用連線時間換取程式碼簡單，先求正確、不做這個最佳化。
- **Hi 旗標（>600 mg/dL）／品管測試（Control Solution）都直接濾掉不上傳**，跟 FORA 的無效讀值／QC 過濾邏輯處理原則一致，保守起見不猜測協定文件沒講清楚的欄位語意。

**2026-08-28 第一次實機測試結果**：燒錄後裝置能正確掃到 GM700SB（廣播名稱 `2782WBC1987(C)`，Service UUID `0xFEE0` 粗篩成功）、送出連線請求、`GAP_SUBEVENT_LE_CONNECTION_COMPLETE` 成功——**但 `sm_request_pairing()` 之後配對失敗，`SM_EVENT_PAIRING_COMPLETE` 回傳 `status=0x08`**（藍牙 SM 協定「Unspecified Reason」）。原因目前不確定，比較可能的方向：(1) GM700SB 可能要求 host 端也開 bonding 才接受配對——現有程式碼刻意不開 bonding（`sm_set_authentication_requirements(0)`，見 `mode_ble_receive_run()` 的說明，是避免 FORA 那種「裝置沒真的存 bonding 資訊、下次連線金鑰對不上卡住」的風險），但協定文件的 BLE Pairing Flow 圖有畫到「Paired data already exists?」分支，暗示官方 App 端有做 bonding；(2) 裝置本身可能需要先在自己螢幕上有動作才會接受配對請求。**下次接續開發要先確認這個根因，可能需要先試著開 SM_AUTHREQ_BONDING 看配對會不會成功**。

**2026-08-28 同一輪測試順便抓到並修好一個共通漏洞**：配對失敗（`SM_EVENT_PAIRING_COMPLETE` 的失敗分支）原本完全沒有設定 `s_kind_cooldown_until`，斷線後立刻恢復掃描，如果失敗的裝置還在附近廣播（GM700SB 就是），會立刻又重新掃到、立刻又重連、立刻又配對失敗，變成無限快速重試迴圈——這不是 GM700SB 專屬的問題，血壓計/MD6 走的是同一段配對程式碼，只是之前配對一直成功、沒機會暴露。已修好並實機驗證：配對失敗時比照成功讀到資料後一樣，套用 `DEVICE_RECONNECT_COOLDOWN_MS[kind]` 當冷卻時間，2026-08-28 燒錄後實測配對失敗到下次重連間隔確實是 10 秒，不再無限快速重試。

**2026-08-28 第二次實機測試：開 bonding 後配對還是失敗，但發現關鍵線索**——先試著在連線後臨時開 `SM_AUTHREQ_BONDING` 再呼叫 `sm_request_pairing()`（送完立刻改回 0，不影響血壓計/MD6），結果**配對足足等滿 30 秒才失敗**（`status=0x08`），跟第一次幾乎立刻失敗不一樣——代表 **GM700SB 完全沒有回應我們主動送出的 Pairing Request**，是被動逾時，不是主動拒絕。回頭比對協定文件附錄的 BLE Pairing Flow 圖，流程圖第一步是「Security Request」——這在藍牙標準用語裡是**周邊裝置主動送給中央端**要求加密的封包，不是中央端主動送配對請求，跟血壓計/MD6「連線後我們主動配對」的模式相反。

同一輪測試也發現：中途有一次配對失敗顯示 `status=0x13`，這不是 GM700SB 的真實回應，是我們自己的「還沒校時成功、開機後第一次主動觸發 UPLOAD 補校時」機制把 BLE 連線強制拔斷造成的雜訊（Pico W 天線 WiFi/藍牙互斥，切模式前會強制斷線）——之後分析 log 要留意排除這種跟目標裝置無關的干擾。這次也順便看到 UPLOAD 嘗試連 WiFi 全部認證模式失敗（`rc=-8`），這是 PROJECT_PLAN.md 早就記錄過的既有問題（密碼待透過 AP_CONFIG 重新設定），跟 GM700SB 無關。

**2026-08-28 第三次嘗試：不主動配對，改成直接探索——進展一大步，找到真正根因**。改成連線當下不呼叫 `sm_request_pairing()`，直接 `discover_rightest_service()` 開始探索。實機結果：**服務/characteristic 探索完全不需要加密就成功**（`FEE1/FEE2/FEE3` 三個都正確找到），**訂閱 `FEE2` Notify（寫 CCCD）才第一次失敗，回傳 `att_status=0x05`＝`ATT_ERROR_INSUFFICIENT_AUTHENTICATION`**。這才是真正的根因：GM700SB 不是用 SM Security Request 主動要求加密（第二次嘗試的猜測證實走不通），是用**標準 ATT 層直接拒絕操作**的方式要求加密——這其實是藍牙標準裡「中央端該主動配對」的明確訊號：中央端收到 `INSUFFICIENT_AUTHENTICATION`/`INSUFFICIENT_ENCRYPTION` 才呼叫 `sm_request_pairing()`，不是連線當下就主動配對。

**2026-08-28 第四次嘗試：反應式配對成功了，而且金鑰有正確持久化**——`GATT_EVENT_QUERY_COMPLETE` 收到 `ATT_ERROR_INSUFFICIENT_AUTHENTICATION` 時才呼叫 `sm_request_pairing()`，實機測試**配對真的成功了**：裝置螢幕跳出配對確認畫面，使用者在裝置上按了實體按鍵確認，`SM_EVENT_PAIRING_COMPLETE` 回 `ERROR_CODE_SUCCESS`。而且因為有開 `SM_AUTHREQ_BONDING`，**金鑰有正確持久化**——後續好幾次重新連線完全不需要再走一次配對流程（沒有再出現 pairing 相關 log），bonding 的假設證實成立。這證明第 1 點（裝置螢幕沒有配對提示只顯示藍牙圖示）那次觀察，其實是「還沒真的走到會跳確認畫面的那個時間點」，不是裝置不需要使用者確認——**GM700SB 實際上需要使用者在裝置上手動按鍵確認配對**（不是純軟體 Just Works），這點列入之後燒錄測試的操作 SOP：實機測試時要留意裝置螢幕、需要時按鍵確認。

**同一輪測試發現新的卡點**：配對成功後，`enable_value_updates()` 完成訂閱的同時送出「PCL 開啟」寫入指令，緊接著收到裝置自動推播的 meter-ID Notify、又立刻嘗試送「查詢型號」指令——**兩個 ATT 請求撞在一起**（同一條連線一次只能有一個在途的請求），第二個送不出去，裝置從此沒再收到任何指令，卡在 PCL 鎖定畫面（實機觀察：裝置螢幕一直顯示游標動畫）直到連線逾時（16 秒，`HCI reason=0x08`）斷線，接著無限重複這個循環。

**2026-08-28 第一次修法：整台裝置當機，已還原**——改成完全序列化，同時把 `finish_rightest_session()`（收尾關閉 PCL）也改成要等它自己的 ATT 回應才真的斷線（不是送完立刻斷線）、`handle_rightest_notification()` 的 `default:` 兜底分支也會處理到新加的狀態。燒錄後**整台裝置完全沒有任何序列埠輸出**（開機/掃描的 log 都沒有），拔插 USB 完整斷電重開機也一樣——不是「卡在某個狀態」，是徹底無回應，懷疑是新狀態在某個時機被 `handle_rightest_notification()` 的 `default:` 兜底到、間接讓 `finish_rightest_session()` 在前一個寫入還沒確認完成時又送一次寫入，撞到 BTstack 內部某個「不該同時有兩個進行中請求」的保護機制導致整個當機（不是單純的邏輯錯誤，推測是更底層的當機，還沒有進一步查證根因，只確認了「改回舊版就恢復正常」）。已經還原成舊版（`git diff` 驗證 `PCL_ON_PENDING`/`PCL_OFF_PENDING` 兩個字串都已清除），確認序列埠輸出恢復正常。

**2026-08-28 第二次修法：裝置沒有再整台當機，但實機測試發現另一個獨立問題**——用第一次修法的縮小版（只加 `RIGHTEST_SESSION_PCL_ON_PENDING`、明確 `case` 處理不依賴 `default:`、`finish_rightest_session()` 完全不動）燒錄後，裝置正常運作，收到 meter-ID 自動推播（16 bytes Notify），**但狀態機沒有被推進**，`enable_rightest_notify()` 之後就再也沒有下一步 log（沒有「got meter-ID push...」）。查了原始封包：`0e 32 37 38 32 57 42 43 31 39 38 37 28 43 29 2a`，解出來是 `[0x0E]"2782WBC1987(C)*"`——**這 16 bytes 完全沒有帶協定文件說的 2-byte 分包表頭（`[總封包數][目前第幾包]`），是內容原始直接送**，`rightest_reassembly_feed()` 誤判成表頭格式不符、直接丟棄整包，`handle_rightest_notification()` 因此提前 return，狀態機永遠卡在 `WAIT_METER_ID`。

**已修好（已寫完程式碼、編譯過關，還沒實機驗證）**：
1. `handle_rightest_notification()` 對 `RIGHTEST_SESSION_WAIT_METER_ID` 完全跳過重組邏輯——反正這則推播的內容本來就忽略，收到任何 Notify 直接當「可以送 PCL 開啟」的訊號，不再嘗試解析。
2. `rightest_reassembly_feed()` 加一個判斷：**payload 第一個 byte 是不是 Return Data Header（`0x4F`）**，是的話直接當成已經收完整的單包回應（不跑 2-byte 表頭邏輯），不是才照文件說的分包表頭處理——因為不確定型號/總數/單筆記錄這幾個單包就裝得下的正式指令回應，是不是也跟 meter-ID 推播一樣省略表頭（協定文件的表頭說明適用範圍寫得不夠精確），用「回應內容本身是不是真的以 `0x4F` 開頭」這個可驗證的訊號來判斷，比死板地假設「一律有表頭」更穩健，兩種情況都能正確處理。

**2026-08-28 實機驗證：前兩項都過關，卡在第三項——型號查詢完全收不到回應**。加了除錯 log 印出實際寫入 FEE3 的原始 bytes 跟 ATT 層回應狀態後確認：`b0 00 b0`（跟協定文件範例完全一致）**寫入本身在 ATT 層永遠成功確認**（`gatt_client_write_value_of_characteristic()` 呼叫成功、對應的 `GATT_EVENT_QUERY_COMPLETE` att_status 也是 SUCCESS），但裝置**從沒回過任何 Notify**，17 秒後連線逾時斷線，重複三次都一樣。裝置螢幕全程維持在游標動畫，沒有變化。排除了 ATT 碰撞、handle 寫錯、checksum 錯誤這幾種可能——寫入確實送達裝置，只是裝置的應用層沒有回應。

**根因**：回頭比對協定文件附錄「Measurement Data transmission Flow」流程圖，官方的資料同步流程是 **PCL 開啟後直接送「讀取總筆數/書籤」（`0xB0 0x61 0x00 0x00`），中間完全沒有經過型號查詢這一步**——型號查詢是文件「Protocol Description」節單獨列出的指令範例，不是這個資料同步流程的一部分，GM700SB 進入 PCL 鎖定狀態後很可能根本沒實作/不理會這個指令。

**已修好（已寫完程式碼、編譯過關，還沒實機驗證）**：拿掉型號查詢這一步，改成照官方流程圖走——PCL 開啟自己的 ATT 回應到了之後直接送 TYPE 1（讀取總筆數/書籤）查詢。連帶影響：**掃描階段的 Service UUID 0xFEE0 粗篩變成唯一的身份確認機制**，不再有連線後型號字串二次核對這道保險（`rightest_protocol_parse_model_name()`/`RIGHTEST_CMD_QUERY_MODEL_NAME` 相關程式碼還留在 `rightest_protocol.c/.h` 裡沒刪，只是沒接進 `mode_ble_receive.c` 的流程，之後如果想用在其他用途還在）。**下次接續開發先燒錄測試，這次要看的重點依序是**：(1) 裝置還是不會整台沒反應；(2) PCL 開啟後能不能直接收到 TYPE 1 回應（總筆數/書籤）；(3) 逐筆讀記錄能不能一路走完、解析出實際血糖數值。

**2026-09-01 實機驗證：完整讀到血糖機的 3 筆舊記錄，整個讀取流程第一次端到端跑通**。上面三個待驗證重點全部過關：(1) 沒有再發生整台無回應；(2) PCL 開啟後直接收到 TYPE 1 回應（總筆數/書籤）；(3) 逐筆送 TYPE 2 讀記錄一路跑完，3 筆記錄都正確解析出血糖數值。連帶再次確認配對路徑（2026-08-28 第四次嘗試的反應式配對）穩定可用。**還沒確認的是這 3 筆記錄有沒有進一步成功存進 flash、上傳到後端**（見下面第 7 項），下次接續要看這段。

**2026-09-01 新增每天定時自動同步**（`RIGHTEST_AUTO_SYNC_HOUR`，`mode_ble_receive.c`）：居家照護情境下個案每週才被護理人員訪視一次，純手動 KEY1 觸發會讓資料同步頻率退化成一週一次，跟每日量測的目標不符；但 GM700SB 廣播內容看不出有沒有新資料，貿然全自動連線只是重新引入 2026-09-01 稍早才拿掉的「盲目輪詢」問題（見上面「使用者決定」那段）。折衷做法：**每天固定 22:00（使用者選定，多數人這時候睡前血糖已經測完）自動打開一次跟 KEY1 短按完全相同的 30 秒同步視窗**（直接複用 `s_rightest_manual_sync_active`/`_until`，等同「軟體幫使用者按一次 KEY1」，不是另開一條連線路徑），用本地日期（`wall_clock_to_epoch_ms()` + `LOCAL_UTC_OFFSET_SEC` 換算）判斷今天是否已經觸發過，避免同一天重複觸發；必須等 `wall_clock_is_synced()` 為 true 才生效。KEY1 手動觸發完全不受影響，護理人員訪視時仍可隨時短按強制同步。**還沒實機驗證**（已編譯成功）——下次燒錄測試時留意 22:00 前後的 log，確認視窗有準時打開、且沒有跟其他自動行為（NTP 重試觸發 UPLOAD 導致 BLE 斷線等）打架。

**2026-09-18 實機驗證「廣播內容看不出有沒有新資料」這個假設，結果：假設成立，維持現有設計**。改用 Pico 2 W（RP2350）實機測試（燒錄過程另外踩到 picotool.exe 在這台機器上執行 `uf2 convert`/`coprodis` 會 Segmentation fault 的問題，改用 `tools/bin2uf2.py` 手刻繞過，見該檔案開頭註解），用 `debug_print_advertisement()` 既有的 `raw_adv` 除錯輸出（見上面第 403 點），比對「閒置中」跟「實際用試紙做兩次血糖量測」前後的原始廣播封包：閒置持續監看約 3 分鐘（`tools/idle_baseline.log`，92 筆樣本）加上使用者自己另外監看視窗記錄的約 300+ 筆樣本，涵蓋兩次真實量測前後，**所有樣本的 `raw_adv` 內容一路都是同一組 hex（`02010607030a18f5fee0fe0f093237383257424331393837284329`），只有 RSSI 訊號強度在變動，量測動作本身完全沒有讓廣播內容產生任何差異**（不是理論推測，是兩次真實量測都驗證過）。結論：GM700SB 沒有辦法只靠被動掃描廣播分辨「有沒有新資料」，現有的手動 KEY1 30 秒視窗／每天 22:00 定時觸發這個折衷設計是必要的，不能改成比照 FORA 系列掃到就直接連線。

同一輪測試接著按 KEY1 觸發手動同步，順便重新驗證了第 427 點在 Pico W 上測到的配對行為，在 Pico 2 W 上結果一致：這台 GM700SB 之前沒跟這台裝置配對過（新板子、littlefs 分區全新，沒有既有 bonding 金鑰），連線後如預期先探索 service/characteristic 成功、訂閱 FEE2 Notify 被拒絕（`att_status=0x05`）、觸發反應式配對——**使用者在血糖機本體上手動按了確認鍵**才配對成功（log 上「開始配對」到「實際開始讀資料」中間有約 24 秒空檔，對應這段實體按鍵操作的時間），配對成功後 PCL 開啟、讀到記錄、解析出 `type=GLUCOSE value=569.0 mode=GEN`，收尾斷線一樣是已知無害的 `att_status=0x1f`。**再次證實 GM700SB 需要使用者在裝置本身手動確認配對，不是純軟體 Just Works，這點不受換成 Pico 2 W 影響**。**2026-09-22 使用者確認**：569.0 mg/dL 這個數值跟血糖機本體螢幕上顯示的數字一致，解析正確無誤。**補充**：569.0 mg/dL 是這台測試用血糖機從很早以前就存在的一筆舊記錄（使用者確認「一直是最舊的記錄」），不是這次測試才產生的——之後如果在其他輪測試的 log 裡又看到這個數值出現，屬於正常現象（這台測試機的歷史記錄裡本來就有這一筆），不代表定位點/去重邏輯有問題。

**2026-09-24 實機發現新的配對失敗模式：換板子（Pico W → Pico 2 W）後跟舊裝置的 bonding 不對稱，反應式配對這條路目前救不回來**。這次測的是另一台 GM700SB（序號 2782SAD1295，位址 `B0:C2:05:17:13:A7`），跟第 452 點驗證過的是不同台裝置——那台是「從沒配對過」的全新配對，這次是「裝置那端還記得舊 Pico W 的配對紀錄，新的 Pico 2 W 認不得」的情境，第 6.6 節開頭已經記載過這個 `att_status=0x1f` 情境（2026-09-02），但當時沒有實機驗證過反應式配對能不能真的救回來。這次的實測結果：
```
13:07:42.407  KEY1 pressed, listening for GM700SB for the next 30s...
13:07:43.773  matched device...connecting
13:07:56.321  connected（連線建立花了近 13 秒，偏慢）
13:07:56.543  rightest: att_status=0x1f at state=3, requesting pairing then retrying...
13:07:56.552  disconnected (reason=0x3e)
```
`sm_request_pairing()` 送出後裝置直接斷線，30 秒視窗內沒有再重新連線成功，之後也沒有任何自動重試（視窗已過期、要等下一次 KEY1 或隔天 22:00）。同一輪測試另一台 D40 血壓計也测到類似現象，只是錯誤碼不同（`pairing failed (status=0x13)`，Remote User Terminated Connection，發生在 `SM_EVENT_PAIRING_COMPLETE` 階段，比 GM700SB 這次更早就被裝置端拒絕）。**兩者共同點**：都是 Pico 2 W 換板子後、跟先前用舊 Pico W 板子配對過的實體裝置之間的 bonding 不對稱，`sm_request_pairing()` 這個軟體端重新要求配對的方案，實測至少對這兩台裝置沒有成功救回連線。目前判斷比較務實的解法是要在裝置本身（GM700SB／D40）清除舊的配對記錄，但還沒找到官方文件裡有沒有這個選項，也還沒實機驗證清除後能不能重新配對成功——這是下次要接續驗證的項目，見下方新增的待辦。

**2026-09-24 實機測到 GM700SB 閒置自動關機的時間點：斷線後約 27 分 24 秒**。針對上面同一台裝置（2782SAD1295），配對失敗斷線（13:07:56.552）之後改用序列埠監聽持續追蹤它的背景廣播訊號（不透過 Gateway 的掃描比對邏輯，直接看原始 `[BLE scan]` debug log），分四輪、合計約 40 分鐘觀察：前三輪（每輪約 10 分鐘，涵蓋斷線後 0~30 分鐘）訊號完全沒有中斷，間隔穩定維持在 2~10 秒；第四輪一開始（斷線後約 30 分鐘起）就完全收不到訊號，往回比對確認**最後一次收到廣播是 13:35:20.600**，距離斷線時間點 13:07:56.552 剛好 **27 分 24 秒**，之後持續監聽超過 13 分鐘都沒有再恢復廣播。監聽腳本本身有另外驗證過仍正常持有 COM port（嘗試搶開會回報 access denied），排除是監聽端當掉漏接的可能。**這個數字只有單一裝置、單一次測試的樣本，還沒有交叉驗證過其他台 GM700SB 是不是同樣的門檻**，如果之後又有機會測，可以留意是否穩定落在 27 分鐘上下。

**2026-09-24 重新加回事件觸發自動同步，取代單一 22:00 定時視窗的每日涵蓋率不足問題**：22:00 定時觸發只能可靠涵蓋「22:00 前 27 分鐘內」量測過的裝置（見 27 分鐘自動關機的實測），如果病患不是固定睡前量測，白天量的資料要等到隔天甚至更久才會被 backfill 補上。根因分析：2026-09-02 拿掉「掃到廣播就自動連線」的舊設計，是因為當時冷卻時間只有 60 秒、遠比裝置清醒時間（~27 分鐘）短，導致裝置清醒的整段時間內每 60 秒就被重新連線一次、響一次，不是「事件觸發」這個機制本身有問題。**修法**：`mode_ble_receive.c` 新增「第一次掃到 GM700SB 廣播就排定一個延遲觸發」（`s_rightest_auto_trigger_pending`/`s_rightest_auto_trigger_at`，延遲時間 `RIGHTEST_AUTO_TRIGGER_DELAY_MS`，**使用者 2026-09-24 決定暫時設 0**，之後想留時間給病患收好裝置再連線的話只要調大這個數字），延遲時間到了就開跟 KEY1 短按完全一樣的同步視窗（`s_rightest_manual_sync_active`）；同時把 `DEVICE_RECONNECT_COOLDOWN_MS[FORA_DEVICE_RIGHTEST_GM700SB]` 從 60 秒拉長到**使用者 2026-09-24 決定的 1 小時**，蓋過實測到的 27 分鐘清醒時間，確保同一次清醒期間只會觸發一次、只響一聲。KEY1 手動觸發／每天 22:00 定時觸發兩條路徑原封不動保留當備援，三者共用同一個視窗機制，互不衝突。**已編譯驗證通過（RP2040/Pico 2 W 皆過關），還沒有實機測試**：下次要驗證的重點依序是 (1) 病患量測後，Gateway 真的會在延遲時間到（目前設 0，應該幾乎立刻）觸發連線並響一聲；(2) 同一次清醒期間（~27 分鐘內）不會被重複觸發第二次；(3) 冷卻期過後、裝置下一次真的被使用時，能正常再次觸發，不會被卡死。

**還沒解決／還沒驗證的問題（下次接續開發要做的事，照風險排序）**：
1. ~~配對失敗~~ **已解決（僅限「從沒配對過」的情境）**：2026-08-28 第四次嘗試改成反應式配對（收到 `ATT_ERROR_INSUFFICIENT_AUTHENTICATION` 才呼叫 `sm_request_pairing()`）後成功，金鑰有 bonding 持久化；2026-09-01 實機完整讀完 3 筆記錄再次確認穩定。**但 2026-09-24 發現「換板子後跟裝置舊配對記錄不對稱」這個情境反應式配對救不回來**，見上方新增的說明，這部分還沒解決。
2. ~~`finish_rightest_session()` 送出 PCL OFF 之後沒有等待 ATT 回應就馬上呼叫 `gap_disconnect()`~~ **已確認為無害，不修**（`mode_ble_receive.c` `finish_rightest_session()` 的說明，2026-09-02 已重複驗證多次）：這個關閉寫入的 ATT 回應每次都是 `att_status=0x1f`（沒有正常收到「寫入成功」確認），但裝置螢幕實測正常顯示時間、不是卡在 PCL 鎖定畫面，後續每次重新連線裝置也都正常回應——指令本身有送達，只是斷線時序讓我們這端看不到成功的 ATT 回應，不影響裝置實際解鎖。先不改成等待確認，如果之後真的遇到裝置卡在 PCL 畫面再回頭處理。（另見上面「PCL 自動關機解鎖」的備註，就算指令真的沒送達，裝置自己的逾時也會兜底。）
3. **「last transmission index 是裝置端全域書籤，不是每個連線各自重置」的假設沒有驗證過**——2026-09-01 這次測試是單次連線讀完 3 筆，只驗證了「一次連線內 summary→逐筆讀」這條路徑本身是通的，還沒有機會驗證「書籤是否跨連線保留」（需要分兩次連線，第二次不重讀第一次讀過的記錄才算驗證到）。
4. **廣播階段的 Service UUID `0xFEE0` 粗篩會不會誤配對到其他裝置**——**2026-09-0x 實機已踩到一次**：測試時附近的「Mi Smart Band 6」（小米手環，`raw_adv` 帶 `0xFEE0` service UUID）被粗篩誤判成候選 GM700SB、觸發連線嘗試，`discover_rightest_service()` 找不到 `FEE1`/`FEE2`/`FEE3` 三個 characteristic（`pcl=0 notify=0 write=0`），程式碼正確判斷身份核對失敗並主動斷線（`missing expected FEE1/FEE2/FEE3 characteristic, disconnecting`），沒有造成配對或誤存資料，只是浪費了一次連線時間（幾秒等級）。風險確認是真實存在的，但目前的失敗處理是安全的，不算阻擋部署的問題。
5. **多包重組（`rightest_reassembly_t`）完全沒有實機測資料驗證過**——不確定 GM700SB 實際會不會真的觸發多包（協定文件範例都在 20 bytes 內，TYPE 2 回應剛好 21 bytes，理論上可能剛好卡在單包/多包的邊界，要看實際 ATT MTU 協商結果）。
6. **`RIGHTEST_CMD_QUERY_MODEL_NAME` 回應格式假設 5 個 ASCII 字元**（協定文件範例 `"GM782"`），還沒確認 GM700SB 實機回應的字串長什麼樣子、開頭是不是真的是 `"GM7"`。
7. 上傳到後端的 `dataSource="GM700SB"`、JSON 欄位沿用既有的 `glu_ac`/`glu_pc`（跟 D40/MD6 共用），後端目前應該不需要額外改動就能收。**連線→解析這段 2026-09-01 已實機驗證過**（見上方），但存 flash、實際上傳到後端這兩段還沒確認。
8. **【新增，2026-09-24】換板子後跟裝置舊配對記錄不對稱時，要怎麼真正修好連線**——見上方新增的說明，目前反應式配對（`sm_request_pairing()`）對這個情境沒用。同一輪也在 D40 上測到同性質的失敗（`pairing failed (status=0x13)`）。下次要確認：(1) GM700SB／D40 裝置本身的選單有沒有「清除配對」/「忘記裝置」之類的選項；(2) 如果有，清除之後重新配對是否能成功；(3) 如果裝置本身沒有這個選項，代表換板子後這些舊裝置可能要用別的方式救（例如恢復舊 Pico W 板子的 bonding 資料，但目前沒有工具能匯出/匯入 BTstack 的配對金鑰，見 PROJECT_PLAN.md 開頭關於 bonding 資料無法直接清除/搬移的說明）。

9. ~~【新增，2026-09-24】換一台別的個案用過的 D40／MD6／GM700SB 給新個案時，裝置內殘留的舊個案記錄會被誤植上傳~~ **已實作（2026-09-24），還沒實機驗證**：這三種裝置都有「往回翻頁補歷史」的 backfill 機制（見第 6.3/6.5/6.6 節），換一台「別人用過」的實體裝置給新個案時，Gateway 對這台裝置的藍牙位址完全沒有既有同步點，第一次連線會把裝置裡存的歷史記錄整批往回抓（GM700SB 最多 500 筆、D40/MD6 最多到 `RECORD_BACKFILL_SAFETY_CAP`=1500），如果這些是別的個案留下的資料，會被當成新個案的資料一起上傳——這是比配對失敗更嚴重的問題：配對失敗頂多連不上，這個是連得上、資料也傳了，但傳錯人。裝置本身沒有清除記憶體的機制可用（GM700SB 確認沒有；D40/MD6 待確認，使用者提出可能可以手動在裝置上清除，但沒有找到官方文件佐證），所以改成韌體層面雙重防護：

   - **主要機制（D）：個案生效時間戳**。`common.h` 的 `device_config_t` 新增 `patient_assigned_since_epoch_ms`（0＝還沒補上）。`mode_ap_config.c` 的 `parse_and_store_form()`：表單送出的 `patient_id` 跟目前已存的不一樣（或這是第一次設定）就把這個欄位歸零；沒變就沿用舊值。`mode_upload.c` 的 `mode_upload_run()`：`wall_clock_sync(8000)` 之後，如果這個欄位還是 0 且校時成功了，補上當下的真實時間、存回 flash（只補一次）。因為 WiFi 帳密跟個案身分證字號是同一份 AP_CONFIG 表單一起送出，設定完成後很快就會嘗試連上真正的網路、觸發校時，正常情況下這個欄位補上的延遲通常只有幾分鐘內，不會拖很久。
   - **保底機制（C）：沒有生效時間戳可比對時，不做歷史回補**。`mode_ble_receive.c` 新增 `record_measured_after_patient_cutoff()`（比對一筆記錄的裝置時間戳是否晚於生效時間戳，生效時間戳是 0 就直接放行，留給下面這個函式把關）跟 `should_limit_backfill_to_latest_only()`（生效時間戳是 0 且這台裝置的位址從沒同步過，才回傳 true）。生效時間戳「已知」時，D40/MD6 的 `handle_record_backfill_notification()`（`RECORD_BACKFILL_WAITING_PART_B`）跟 GM700SB 的 `RIGHTEST_SESSION_WAIT_RECORD` 在每翻一筆記錄時都會檢查，翻到生效時間之前的記錄就直接停手斷線（跟翻到既有同步點的處理方式一樣），不採信這一筆也不繼續往回翻。生效時間戳「未知」+ 沒有既有同步點時，`process_reading_payload()`（D40/MD6 的 index=0）跟 GM700SB 的 `s_rightest_target_count` 都會被限制成只收「這次連線當下最新那一筆」，不觸發 backfill 往回翻頁——寧可保守漏掉個案自己在生效前量的極早期幾筆，也不要誤植別的個案的資料。
   - 兩者合起來：正常路徑（生效時間戳很快就補上）資料最完整，異常/邊界情況（校時延遲、WiFi 密碼設錯）也不會犯「誤植別人資料」這種更嚴重的錯，只會保守地漏資料，之後同一台裝置只要同步成功一次、有了同步點，就不再受這條規則限制。
   - **追加修正（同一天發現的漏洞）**：backfill 同步點是依 (裝置種類,藍牙位址) 存的，跟個案完全無關——一開始的實作漏了一件事：如果換個案時不順便清掉同步點，舊個案用過的實體裝置換給新個案時，這台裝置的位址還留著舊的同步點，`should_limit_backfill_to_latest_only()` 會因為「有既有同步點」誤判成「這台裝置在目前個案底下已經同步過」，讓保底機制被繞過——在生效時間戳補上之前那段空檔（通常幾分鐘）完全沒有防護，舊個案的資料可能被當成已同步、不會被擋下來。**已修正**：`storage.c` 新增 `storage_clear_all_backfill_anchors()`（列出所有 `bfa_*.bin` 同步點檔案、全部刪除），`mode_ap_config.c` 偵測到 `patient_id` 變更時，歸零生效時間戳的同時也呼叫這個函式清空全部同步點。清完之後即使是舊個案用過的裝置，也會被當成「這台裝置對目前個案來說是第一次同步」，正確觸發保底機制。
   - **C 保底機制殘餘的已知風險（刻意接受，不是 bug）**：就算同步點清乾淨了，如果新個案還沒真的用過這台裝置就被連線，C 保底機制仍然會收下裝置當下回報的「最新一筆」——這一筆有可能還是舊個案最後留下的值，不會被過濾掉。這是 C 保底設計上的取捨（寧可最壞情況誤收一筆，也不要整批歷史外洩），跟同步點清除是兩個獨立的問題。
   - **已編譯驗證通過**（RP2040 `build/` 與 Pico 2 W `build-pico2/` 皆過關），**還沒有實機測試**：下次要驗證的情境依序是 (1) 換一台全新個案，確認 `patient_assigned_since_epoch_ms` 在校時成功後真的被補上；(2) 拿一台已經有歷史記錄的裝置在生效時間戳還沒補上前就連線，確認只收到一筆、沒有整批回補；(3) 生效時間戳補上之後，拿裝置模擬「有記錄早於生效時間、也有記錄晚於生效時間」，確認只有晚於的那些被收進來、backfill 在跨過生效時間點時正確停手。

## 7. 待辦事項

分成兩類：**7.1 軟體層面**是需要寫新程式碼/改邏輯的（功能還沒做完或有已知的正確性風險），**7.2 測試/驗證層面**是程式碼已經寫好、需要實際跑一遍確認正確的。兩者優先順序不互相排斥，可以平行進行。

### 7.1 軟體層面待辦（需要寫新程式碼／改邏輯）

**2026-08-06 更新：這一節列出的 7 項當時都還沒寫，這次裝置不在身邊沒辦法實機操作，趁機把全部 7 項的程式碼都寫完、編譯過關了，細節見第 8.5 節。全部還沒燒錄、更沒有實機測試過，下面只留還沒寫程式碼、或者程式碼寫完但還需要真正決定值/憑證才能算完成的項目：**

1. **TLS 憑證驗證框架已接好，但還沒有真正的憑證可以放**：見 `upload_tls_ca_cert.h`，`UPLOAD_CA_CERT_PEM` 目前是空字串，upload_api.c 會照舊用不驗證模式（並在 log 印警告）。等正式後端網址確定之後，把該伺服器的憑證/CA PEM 貼進這個檔案就會自動改成真正驗證，不用改任何其他程式碼——**這個檔案本身沒有東西要寫了，純粹是在等一個「正式後端網址」的決定**。
2. **上傳伺服器網址/認證金鑰目前用 AP_CONFIG 表單設定，但伺服器端還沒有實作認證檢查**：`upload_api.c` 已經會在有填認證金鑰時送出 `X-API-Key` 標頭，但這是單邊的——目前唯一的測試伺服器 `test_server/app.py` 完全沒有檢查這個標頭（也不應該檢查，它就是設計給沒有認證的本機測試用）。正式後端要自己實作驗證這個標頭的邏輯。
3. **FORA MD6/D40 支援：廣播辨識/連線/配對/正式解析/多筆記錄往回翻頁/flash 持久化同步基準點/AC-PC-GEN 情境解析/Hb 換算全部已實作**，D40 往回翻頁跟 AC/PC 解析已實機驗證，flash 持久化基準點的「跨連線接力」路徑還沒實機驗證過（目前測過的裝置一次連線就能追完）。還沒解決的是「Ketone/UA/CHOL 數值 scale 未驗證」，跟卡住上傳驗證的 WiFi 密碼問題（使用者決定暫不處理），見第 6.5 節完整發現記錄。
4. 見第 9 節「已知限制」剩下還沒有處理的項目。

### 7.2 測試/驗證層面待辦（功能已經寫好，需要實機測試確認正確）

1. ✅ 血壓計完整「BLE 收到 → WiFi 上傳」流程：連線→配對→兩段式取記錄→解析→上傳，數值跟裝置螢幕一致。
2. ✅ 血壓重複上傳同一筆記錄：改用裝置量測時間戳判重＋血壓計冷卻拉長到 4 分鐘，同一筆舊記錄不會重複上傳、真正新的量測能正確被當成新資料。
3. ✅ **~~血壓計 4 分鐘冷卻是否真的讓裝置自動關機~~（2026-08-05 晚間約 19:30 燒錄的版本已實機確認）**：使用者確認那個版本（血壓計冷卻 4 分鐘）裝置真的會自動休眠，拉長冷卻時間讓裝置撐過 3 分鐘門檻的假說成立。
4. **NTP 校時成功率／定期重新校時邏輯**：已修過「成功一次就永遠跳過重新校時」的正確性 bug，改成每 6 小時最多真的打一次網路（見 `wall_clock.c` 的 `NTP_RESYNC_INTERVAL_MS`）。**還沒實機測試過**：需要確認第一次開機時仍然能正常校時成功、且連續運作超過 6 小時後真的會觸發一次重新校時。
5. **24 小時等級耐用性測試**：這是這次交接的主要目的。讓裝置長時間連續運作（例如放床邊，斷續有真實裝置量測、斷續上傳），觀察：
   - 是否會卡死、無回應（需要重新插電才會恢復）。
   - flash 寫入次數多了之後是否穩定（已改用 littlefs 做 wear-leveling，見第 5.1 節，但這部分本身也還沒實機測試過）。
   - WiFi 連線成功率、是否會遇到偶發性的 `NONET` 快速失敗（測試過程中觀察到過，原因未深入排查）。
   - ✅ 斷電測試（2026-08-06）：量測後、還沒上傳成功前故意拔電源，重開機後（序列埠一度消失又重新列舉，`DEV_Module_Init OK` 正常開機）待傳資料順利在下次連上 WiFi 時觸發上傳並成功，沒有資料遺失；這次剛好也測到 NTP 兩台伺服器都逾時的情況，正確 fallback 用 boot-relative 時間戳送出，驗證了 NTP 完全失敗時的退回邏輯。這次沒有搭配littlefs 重開機持久化的完整交叉比對（見第 7.2 節第 15 點，仍待驗證的是「重開機後三份 littlefs 檔案內容本身」的細節，不是這裡驗證的「待傳資料在斷電情境下最終有沒有送到伺服器」）。
6. **多裝置情境**：目前只驗證過「一次一種裝置在旁邊」，沒測過三種裝置同時在附近廣播、輪流量測的情境（連線順序、掃描優先權、是否會互相干擾）。
7. **不同 WiFi 環境**：目前主要測試環境是家用路由器（WPA2）跟 iPhone 個人熱點。實際部署環境（醫院/病患家中）的路由器認證模式可能不同，程式碼已經做了多種認證模式輪流嘗試（見 `mode_upload.c` 的 `WIFI_AUTH_MODES_TO_TRY`），但沒有大量現場測試過。
8. ✅ **WiFi QR code 手機掃碼測試（2026-08-06）**：實機掃碼確認會跳出「加入 WiFi」的系統提示，QR code 編碼/`draw_qr_code()` 縮放繪製都正常。
9. ✅ **「Scanning」心跳時間戳的畫面視覺驗證（2026-08-06）**：見第 12.6 節，實機肉眼確認面板上的時間戳在超過 180 秒沒有裝置廣播的安靜期間仍會自動前進，證實心跳機制正常運作、能正確分辨「裝置還活著只是沒掃到裝置」跟「裝置當機畫面凍結」。
10. **電子紙連續讀值即時更新／24 小時強制刷新驗證**：還沒確認 BLE_RECEIVE 連續收到好幾筆讀值時畫面是不是每次都有即時更新，也還沒驗證過 `MAX_REFRESH_INTERVAL_MS`（24 小時強制刷新）的邏輯有沒有真的觸發過（可以先改成短時間測試）。
11. ✅ **血糖協定實機驗證（2026-08-06 燒錄後實機測試，完整驗證通過）**：真的用 D40 量測一次，`byte[2]` bit7 正確判斷成血糖（bit7=0）、`byte[4..5]` 正確解出 107 mg/dL，**跟裝置螢幕實際顯示的數字比對一致**，完整走過 BLE 解析→判重→存 flash→上傳到伺服器全部成功。協定第 6.4 節的欄位說明（反編譯 `BloodGlucose2in1` class 得出的格式）到此已實機證實正確，不再是只比對過原始碼的未驗證狀態。
12. ✅ **AP 熱點密碼實機驗證（2026-08-06，改為固定值後）**：密碼固定是 `02750963`，不用再驗證「螢幕顯示的密碼是不是跟實際能連線的密碼一致」這種每台裝置不同的比對，本身就是固定文件已知的值。
13. **【新增】血壓計時鐘合理性檢查實機驗證**：`mode_upload.c` 已經接上「跟 Pico 現在時間比對，差距過大就不信任裝置時鐘」的邏輯（見第 8.5 節），還沒有實機測試過兩種情境：(a) 裝置時鐘正常時是否還是正確採用裝置時間戳；(b) 刻意製造裝置時鐘異常（如果測試裝置支援改時間）時是否正確退回 Pico 收到時間。
14. ~~電子紙局部刷新（Phase 3）實機驗證~~**（2026-08-06 已移除，不再需要驗證，見第 12.5 節）**：局部刷新曾經接上又拿掉了——沒有任何實測觀察到的具體場景真的需要它，換來的是沒有實機驗證過的呼叫順序風險跟疊代殘影需要的額外清理邏輯，為一個從未真正發生過問題的場景背負未測試的複雜度不值得，決定只保留全刷。
15. 🔧 **littlefs flash 持久化實機驗證（部分驗證，2026-08-06）**：見第 5.1/8.5 節第 28 點。這次燒錄後開機正常掛載（沒有卡在 `lfs_mount()`/`lfs_format()` 失敗）、AP_CONFIG 存的設定/BLE 收到的待傳紀錄/上傳成功後的歷史三份資料整個測試過程都正確讀寫，且中途經歷一次意外斷電重開機（見上面第 5 點斷電測試）後裝置也正常恢復運作，間接印證 littlefs 掛載本身是穩固的。**但還沒有針對性驗證**：(a) 全新機器第一次燒錄時 `lfs_format()` 自動觸發的那一刻；(b) 特地重開機比對「重開機前後三份資料內容逐筆一致」這種精確比對；(c) 長時間大量寫入下的穩定性（可以搭配第 5 點的 24 小時耐用性測試一起做）。
16. ✅ **KEY0/KEY1 按鍵實機驗證（2026-08-06）**：KEY0 長按 3 秒確認會切去 AP_CONFIG；KEY1 按一下確認會立刻觸發上傳（含佇列非空跟後續測到的情境），兩者都沒有影響 BOOTSEL/idle timeout 路徑。
17. **【新增】無螢幕版本相容性實機驗證**：見第 12.9 節。這次測試的機器有接電子紙板，**還沒有拿一顆沒接電子紙板的 Pico W 實際驗證過**；也要確認面板正常接上時，`EPD_BUSY_PIN` 加的內建下拉/`ReadBusy()` 的 5 秒逾時沒有意外拖慢或影響正常刷新（這次有接面板正常運作，間接印證後者沒問題）。
18. ✅ **KEY2 歷史畫面實機驗證（2026-08-06）**：按 KEY2 確認會顯示最近已上傳歷史（測到 6/6 筆），數字隨上傳增加正確更新；**8 秒逾時自動換回 BLE_RECEIVE 即時畫面也已確認正常**。
19. ✅ **KEY1 強制 NTP 重新校時實機驗證（2026-08-06）**：確認按 KEY1 時 log 印出「forced resync requested」、真的重新查詢 NTP 伺服器（`pool.ntp.org` 失敗後換 `time.cloudflare.com` 成功），就算待傳佇列非空也會這樣做；也確認同一輪測試中 idle-timeout 觸發的上傳路徑正確沿用既有校時基準（沒有強制刷新），行為符合預期。
20. ✅ **KEY0 取消 AP_CONFIG 實機驗證（2026-08-06）**：在 AP_CONFIG 畫面按住 KEY0 確認 log 印出「KEY0 held, cancelling without saving」、正確退回 BLE_RECEIVE 並恢復掃描；3 秒長按（進入用）跟取消用的門檻沒有互相干擾。
21. ✅ **NTP 一直沒校時成功時的自動重試實機驗證（2026-08-06）**：確認 5 分鐘後 log 印出「wall clock still unsynced, triggering WiFi to retry NTP」、正確主動觸發一次 WiFi 連線重試校時；也發現並修好了「開機後第一次還沒校時成功時，要等滿一整個 5 分鐘週期才會第一次嘗試」這個問題，見第 8 節第 39 點。
22. ✅ **AP_CONFIG 設定網頁實機測試（2026-08-06）**：加簡約手機版 CSS 時踩到 printf 格式化字串的坑（見第 8 節第 40 點），一度讓網頁打不開/captive portal 偵測失敗，最終決定整個復原成原本的版面，只保留拿掉 dBm 顯示這項改動。復原後已實機確認手機能正常自動跳出設定頁面。
23. **【新增】電子紙 AP_CONFIG 畫面 SSID/密碼改用 Font8 顯示**：見第 8 節第 39 點，`SSID: GATEWAY-XXXX` 改短之後 + 改用 Font8，理論上不會再超出 QR code 的區域，但還沒有肉眼確認電子紙螢幕上的實際排版（今天的測試確認的是手機網頁那一側，不是電子紙螢幕畫面本身）。

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
7. **待傳生理資料原本只存在 RAM，斷電就遺失**：**修法**：整份寫進 flash（`storage.c` 的 `persist_pending_records()`），開機時讀回。原本沒有 wear-leveling，每次新增一筆或上傳結果都會整份覆寫一次；後續已改用 littlefs 解決，見第 5.1/8.5 節第 28 點。
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
28. **Flash 持久化改用 littlefs，解決 wear-leveling 問題**：見第 5.1 節完整說明。原本 `storage.c` 手刻三個固定 sector 沒有 wear-leveling，改成掛載 littlefs（vendored v2.11.3），config/待傳佇列/已上傳歷史變成分區裡的三個檔案，抹寫在分區內多個 block 間輪替。新增 `src/lfs_pico_hal.c/.h`（RP2040 flash block-device shim）、`littlefs/` 目錄（vendored 原始碼）。**一次性、不相容的格式改動**：舊版資料 littlefs 看不懂，第一次掛載會自動重新格式化，等於清空 Pico 上原本的資料——這是刻意接受的代價，見第 5.1 節。只確認過完整重新編譯成功，還沒有實機燒錄測試過。
29. **上傳成功判斷只找回應裡的 "200" 子字串，標頭/內文其他地方剛好出現這三個字元也會被誤判成功**（2026-08-06 通盤 code review 抓到）：舊邏輯 `strstr(header_buf, "200")` 沒有限定只看 HTTP 狀態列，例如錯誤回應的 `Content-Length: 200` 就會被誤判成上傳成功，導致明明失敗的紀錄被標記已上傳、不會重試，實際資料遺失。**修法**：新增 `header_indicates_200()`，只比對狀態列本身（第一個空白後面緊接著 "200" 且下一個字元是空白/`\r`/結尾），不在整段回應內容裡亂找子字串（`upload_api.c`）。
30. **flash 資料損毀/截斷時，`storage_init()` 仍會信任檔頭寫的筆數，把未初始化的假紀錄當成真的待傳資料**（2026-08-06 通盤 code review 抓到）：讀 `pending.bin`/`history.bin` 時沒檢查 `lfs_file_read()` 實際讀到的位元組數，如果資料被截斷，陣列尾端會留著開機時歸零的假紀錄（type=UNKNOWN、status=PENDING），還是會被當成正常待傳紀錄上傳出去。**修法**：檢查每次 `lfs_file_read()` 的回傳值是否等於預期位元組數，不吻合就整份丟棄、印警告，不採信檔頭的筆數（`storage.c` 的 `storage_init()`）。
31. **BLE 連線失敗沒有檢查連線狀態，可能讓狀態機永遠卡住**（2026-08-06 第二次通盤 code review 抓到）：`GAP_SUBEVENT_LE_CONNECTION_COMPLETE` 舊邏輯沒檢查事件本身的 status 欄位，裝置離開範圍等原因導致連線失敗時，不會再有 `HCI_EVENT_DISCONNECTION_COMPLETE` 補上（因為根本沒有真的連上），舊程式碼會直接拿無效的連線 handle 繼續配對/探索，而狀態機在 `BLE_STATE_CONNECTING`/`PAIRING`/`DISCOVER_*` 這幾個狀態完全沒有逾時機制，會永遠卡住、不再回去掃描。**修法**：檢查 `gap_subevent_le_connection_complete_get_status()`，非成功就直接當作失敗處理、重新開始掃描（`mode_ble_receive.c` 的 `handle_hci_event()`）。
32. **GATT 服務/特徵值探索「成功完成」不代表真的找到符合的 UUID，舊邏輯會誤用上一台裝置殘留的 handle**（2026-08-06 第二次通盤 code review 抓到）：`GATT_EVENT_QUERY_COMPLETE` 的 `ATT_ERROR_SUCCESS` 只代表查詢本身正常跑完，陌生裝置沒有這個 service/characteristic 時查詢一樣會「成功」，只是完全沒有結果。舊程式碼沒有分辨這種情況，會直接沿用 `s_fora_service`/`s_fora_characteristic` 裡殘留的、屬於另一種裝置的舊值繼續往下走。**修法**：新增 `s_discovery_found` 旗標，每次開始探索前重置，收到對應的 `QUERY_RESULT` 事件才設成 true，`QUERY_COMPLETE` 時一併檢查這個旗標，沒找到就當失敗處理、斷線（`mode_ble_receive.c`）。
33. **無螢幕版本（不接電子紙板）開機後幾乎必定卡死**（2026-08-06 全盤檢查「無螢幕相容性」抓到，見第 12.9 節完整說明）：Waveshare 原版 `EPD_2IN9_V2_ReadBusy()` 是無限迴圈，沒接面板時 `EPD_BUSY_PIN` 電位未定義、不保證讀得到迴圈要等的值，開機後第一次畫面刷新就可能永久卡住，BLE/WiFi/AP_CONFIG 全部停擺。**修法**：`DEV_Config.c` 幫 `EPD_BUSY_PIN` 加 `gpio_pull_down()`（沒接面板時穩定讀到「不忙」，面板真的接上時會被控制器主動驅動蓋過）；`EPD_2in9_V2.c` 的 `ReadBusy()` 額外加 5 秒逾時上限保底。
34. **KEY2 已上傳歷史畫面**（2026-08-06 加做，尚未實機測試，見第 12.8 節）：`storage.c` 新增 `storage_get_recent_upload_history()`（取最新 N 筆，區別於原本從最舊開始取的 `storage_get_upload_history()`）／`storage_get_upload_history_count()`；`display_status.c` 新增 `display_status_show_upload_history()`（陽春文字列表，最多 7 筆），順便把 `vital_label()`/`vital_unit()` 補上原本缺的 `VITAL_TYPE_SYSTOLIC`/`DIASTOLIC` case（血壓記錄在歷史畫面上原本會顯示 "?"）；`button_input.c` 新增 KEY2 讀取；`mode_ble_receive.c` 主迴圈按 KEY2 觸發顯示、8 秒後自動換回即時畫面，顯示期間暫停呼叫 `display_status_poll()` 避免立刻被蓋掉。這是評估「觸控瀏覽歷史」方案後選的低成本替代做法，見第 12.8 節。
35. **KEY1 手動觸發上傳時，同時強制重新 NTP 校時**（2026-08-06 加做，尚未實機測試，見第 12.8 節）：`wall_clock.c` 新增 `wall_clock_request_resync()`／`s_force_resync` 旗標，`wall_clock_sync()` 檢查到這個旗標就無視距離上次成功校時是否還沒超過 `NTP_RESYNC_INTERVAL_MS`、直接重新查詢，用過一次自動清除。`mode_ble_receive.c` 的 KEY1 分支呼叫這個新函式。原本 idle-timeout 觸發的上傳路徑完全不受影響，還是遵守 6 小時的節流。
36. **KEY1 改成不管待傳佇列是否為空都觸發，且沒資料時也要嘗試 NTP 校時**（2026-08-06 使用者確認「KEY1 = 完整一次 WiFi 動作」的設計後調整）：`mode_ble_receive.c` 拿掉 KEY1 分支的 `storage_pending_count() > 0` 條件；`mode_upload.c` 原本 `wall_clock_sync()` 只在 `count > 0` 時才呼叫，改成不管有沒有資料都會嘗試一次（`count == 0` 時只是不會有東西可以送出去，但校時本身跟有沒有資料無關）。idle-timeout 自動觸發上傳的路徑不受影響，還是要求佇列非空才會切換。
37. **KEY0 進入/離開 AP_CONFIG 的保護程度不對稱，改成離開也要求短暫按住**（2026-08-06 使用者確認後調整）：原本離開 AP_CONFIG 是按一下（`button_input_key0_pressed()`，邊緣觸發）就立刻取消，但取消的後果是直接關掉熱點——如果連著的手機正在網頁表單上填資料，不小心誤觸會讓手機斷線、表單內容全部消失，沒有任何提示，後果比「不小心進入設定模式」更嚴重，門檻卻更低。**修法**：拿掉 `button_input_key0_pressed()`，改成沿用 `button_input_key0_long_press()`（同一個函式，`mode_ap_config.c` 傳 `AP_CONFIG_CANCEL_HOLD_MS`＝1 秒，`mode_ble_receive.c` 進入時傳 3 秒），兩處呼叫追蹤的是同一個實體按鍵的連續按住時間，互不干擾。
38. **裝置一直收不到任何 BLE 讀值的話，永遠沒有機會嘗試 NTP 校時**（2026-08-06 使用者提出後調整，見第 2.4 節完整說明）：`wall_clock_sync()` 只有在 `mode_upload_run()` 被呼叫時才會執行，而觸發 UPLOAD 的條件原本是「收到裝置讀值後才開始倒數 idle timeout」，如果裝置一直沒收到任何讀值就永遠不會主動切去 UPLOAD。**修法**：`mode_ble_receive.c` 新增獨立計時器，不依賴有沒有收到過讀值，只要還沒校時成功、且距離上次嘗試超過 5 分鐘（使用者確認的值，固定頻率不做失敗退避）就主動觸發一次 UPLOAD 嘗試校時。
39. **無螢幕版本使用者拿不到每台裝置唯一衍生的熱點密碼**（2026-08-06 使用者實測反饋後調整）：AP 熱點密碼原本是從 RP2040 board ID 衍生的每台裝置唯一值（見第 24 點），但無螢幕版本沒有任何管道能讓使用者知道衍生出來的密碼是什麼。**修法**：`generate_ap_password()` 改成回傳固定值 `02750963`（所有裝置都一樣，可以事先印在文件/貼紙上）；SSID 仍然維持每台裝置唯一衍生（`generate_ap_ssid()` 不變，只是前綴從 `"PicoGateway-Setup-"` 縮短成 `"GATEWAY-"`，用來分辨機台）。`pico_get_unique_board_id()`／`pico/unique_id.h`／`CMakeLists.txt` 的 `pico_unique_id` 連結都一併移除（不再需要衍生密碼）。
40. **AP_CONFIG 設定網頁加簡約手機版 CSS 時踩到 printf 格式化字串的坑，一度讓設定頁面打不開/captive portal 偵測失敗**（2026-08-06 使用者實機測試抓到）：`html_append()` 內部用 `vsnprintf()` 實作，所有傳進去的字串都會被當成 printf 格式化字串解析。新加的 CSS 內容 `"width:100%;..."` 裡的 `%` 沒有跳脫成 `%%`，`%;` 不是合法的格式化指定字元，屬於未定義行為，導致設定網頁一度打不開、captive portal 也偵測不到。雖然當下就找到並修好了這個特定問題（`%%` 逃脫），但使用者測試時又發現手機出現「無法連上網路」的提示（之前的版本不會），為了不在測試現場冒風險，**最終決定整個復原成加 CSS/`<label>` 結構之前的版面**，只保留「拿掉 WiFi 訊號強度 dBm 顯示」這一項改動（純文字內容變動，不涉及 CSS，沒有格式化字串風險）。**教訓**：這個檔案裡任何要傳給 `html_append()`/`append()` 的字串（包含 CSS、使用者輸入以外的固定字串），只要包含字面上的 `%` 字元，都必須寫成 `%%`，之後如果再嘗試加 CSS／其他包含 `%` 的內容務必注意這點。電子紙 AP_CONFIG 畫面的 SSID/密碼改用 Font8 顯示（見第 22.5 節、`display_status.c`）不受這次事件影響，維持不變。
41. **KEY2 歷史畫面只看得到已上傳成功的紀錄，使用者反應不夠用**（2026-08-26 使用者反應後調整，尚未實機測試）：見第 34 點，原本的 `storage_get_recent_upload_history()`／`display_status_show_upload_history()` 只顯示已經上傳成功、進到環狀緩衝（`s_upload_history[]`）的紀錄，還在待傳佇列裡（`PENDING`/`FAILED`）的紀錄完全看不到。**修法**：`storage.c` 新增 `storage_get_recent_records()`，把待傳佇列跟已上傳歷史兩份資料一起按 `received_at_ms` 排序、取最新 max_count 筆（用小型插入排序暫存陣列取代一次性容納全部紀錄的大陣列，避免 Pico 堆疊塞不下最多 128+200 筆的合併陣列）；`display_status.c` 的 `display_status_show_upload_history()` 改名為 `display_status_show_history()`，每一行前面依紀錄自己的 `status` 欄位（`common.h` 的 `upload_status_t`）加一個單字元狀態標記（`+` 已上傳、`!` 上傳失敗、`.` 待傳中）；`mode_ble_receive.c` 的 KEY2 分支跟著改用新函式，`total_count` 也改成待傳筆數＋已上傳歷史筆數的合計。原本的 `storage_get_recent_upload_history()` 已無其他呼叫端，直接移除。**（2026-08-28 訂正）** 跟程式碼核對後發現這一點描述的修法從來沒有真的落地——`storage_get_recent_records()`／`display_status_show_history()` 這兩個符號在 `storage.c`/`display_status.c` 都找不到，這則筆記是規劃過但沒有實作完成的版本。KEY2 畫面實際上一路維持只顯示已上傳歷史，直到 2026-08-28 才真正改版（見下面第 42 點），改法也不一樣：不是合併顯示，而是反過來預設顯示未上傳、用 KEY2 翻頁。
42. **KEY2 改成預設顯示未上傳紀錄+可翻頁**（2026-08-28，見上一點的訂正說明、`FIRMWARE_FILES.md`「板載按鍵」章節，尚未實機測試）：新增 `storage_pending_records_page(out, max_count, skip)`，`display_status_show_upload_history()` 改名為 `display_status_show_pending_records()`（加 `page_index`/`page_count` 參數），`mode_ble_receive.c` 新增 `s_history_page`：畫面還顯示著時再按一次 KEY2 翻到下一頁，逾時換回即時畫面後再按則從第 0 頁重新開始。
43. **AP_CONFIG 設定頁 HTTP server 「單一連線」設計沒有真的強制執行，導致 logo 圖檔顯示不出來/直接開瀏覽器打網址卡住空白頁**（2026-08-28，實機照片回報後追出來，改過兩版才真正解決）：`http_accept_cb()` 原本不管三七二十一，每次新連線進來就直接 `memset()` 蓋掉唯一一份共用的 `http_conn_state_t state`。手機連上熱點後，除了使用者自己開的分頁，iOS 的 Captive Network Assistant 本身也會在背景定期送連通性檢測請求，這些請求另外開連線，容易跟使用者當下的請求（例如 HTML 回應後緊接著另一個連線去要 `/itri_logo.png`）重疊，重疊時後到的連線一 `memset()` 就把前一個連線正在累積的請求資料沖掉。**第一版修法（實測不夠）**：加一個指標擋掉重疊進來的第二個連線（`tcp_abort()` 拒絕）——重新測試後直接開瀏覽器卡住的問題解決了，但 logo 依然顯示不出來：瀏覽器解析到 `<img>` 標籤幾乎立刻另開連線要圖片，這個時間點常常跟第一個連線的 TCP 關閉握手重疊，「擋掉重疊連線」會讓這個圖片請求被拒絕，瀏覽器對子資源載入失敗通常不會重試。**第二版修法**：改成真的支援小量並發——固定大小（`HTTP_MAX_CONCURRENT_CONN` 3 個）的連線池，每個連線各自獨立的 buffer（`alloc_conn_state()`/`free_conn_state()` 管理），連線池用滿才拒絕新連線；`http_err_cb()`（`tcp_err()` 註冊）確保連線異常結束時也會歸還對應的 slot。**這版燒錄後接序列埠看 log 確認**：`GET /itri_logo.png` 有送達、`is_logo_path()` 有正確比對到、印出 `-> serving logo png (8476 bytes)`——連線收發沒問題，問題在更後面。**第三版修法**：這行 log 是在 `tcp_write()` **之前**印的，只代表「準備要送」，`tcp_write()` 回傳值從來沒被檢查過。`lwipopts.h` 的 `MEM_SIZE`（32768）就是 2026-08-26 因為同一種原因（`tcp_write(..., TCP_WRITE_FLAG_COPY)` 資料太大、`MEM_SIZE` 不夠用，直接回傳 `ERR_MEM` 什麼都沒送出去）調高過一次的（見第 40 點）——logo 圖檔（8476 bytes）獨立成路徑後變成單一最大的一筆 `tcp_write()`，連線池讓同時可能有 3 個連線各自送回應，更容易撞上 `MEM_SIZE`。改成 `tcp_write(..., 0)`（不加 `TCP_WRITE_FLAG_COPY`，`ITRI_LOGO_PNG` 是 flash 常駐 `const` 陣列不需要複製），順便補上回傳值檢查。**燒錄後接序列埠確認**：`GET /itri_logo.png` 這次兩個 `tcp_write()` 都成功、沒有出現失敗 log——但手機那邊還是沒看到 logo，代表問題不在傳輸層。第 43 點這三版修法本身都是真實存在的 bug 也真的修好了（連線互相干擾、記憶體不足），只是都不是 logo 顯示不出來的根因。

44. **logo 圖檔顯示不出來的真正根因：原始碼裡的 PNG bytes 陣列本身已經損壞**（2026-08-28，接續第 43 點，跟程式碼比對三版網路層修法都測過之後才找到）：把 `mode_ap_config.c` 的 `ITRI_LOGO_PNG[]` 陣列還原成實際 PNG 檔案、逐一走過每個 chunk 驗證 CRC，發現 PNG 結構完整（簽章/chunk 長度/IEND 都對），但存畫素資料的 `IDAT` chunk CRC 校驗失敗，`zlib` 解壓縮直接報錯 `invalid distance too far back`——壓縮資料流整個壞掉，無法從這份資料還原正確圖片，跟第 43 點修的網路層 bug 完全無關：就算資料完整送達，手機收到的也是張壞圖，瀏覽器本來就顯示不出來。原始素材 `itri_CEL_A.png` 在這個 repo 裡完全找不到，無法還原，改用使用者提供的 `itri_CEL_C.png`（723x168, 7605 bytes，驗證過結構+CRC 全部正確）重新轉成 C 陣列換掉整個 `ITRI_LOGO_PNG[]`，`ITRI_LOGO_PNG_LEN` 巨集同步改成 7605（目前沒有任何地方真的用到這個巨集，`sizeof(ITRI_LOGO_PNG)` 才是實際算長度用的值，純粹保持數字正確不誤導）。**尚未重新實機驗證**：只確認 `cmake --build` 成功。
45. **表單送出成功頁改成跟設定表單頁統一風格，實機測試後又復原**（2026-08-28，使用者要求）：`SAVED_RESPONSE_HTML` 原本是無樣式的陽春 HTML，先改成套用同一套 logo bar/page-header/card 版面。使用者另外問過要不要改成「送出後彈談窗不跳頁」——評估後維持現在的伺服器端整頁回應（跳頁），因為談窗需要 JS 攔截表單改用 AJAX，這個專案網頁目前完全沒有 JS，且這個表單頁本身 2026-08-06 就曾經因為加 CSS+JS 在實機測試出過問題（見第 40 點），沒有理由現在重新引入。**但加了 CSS 的版本實機測試後使用者回報手機一直出現「無法加入網路」**，跟第 40 點是同一種症狀根因：這個熱點沒有真正的網際網路，作業系統靠連上熱點後對固定探測 URL 的回應內容/大小判斷網路能不能用，任何在連著熱點期間會被請求到的頁面只要加了 CSS/圖片這類會明顯改變回應大小/型態的內容，都可能干擾這個判斷。**已復原成純文字版面**——目前已知這類頁面（表單頁、送出成功頁）不能加豐富樣式，踩過兩次了。

## 9. 已知限制 / 正式上線前必須處理

1. **TLS 憑證驗證框架已接好，但還沒有真正的憑證**（見第 8.5 節第 25 點、`upload_tls_ca_cert.h`）：`UPLOAD_CA_CERT_PEM` 目前是空字串，等於還是不驗證，只是現在會在 log 印出明顯警告、且日後補上憑證不用改程式碼。正式上線前必須把目標伺服器的憑證/CA PEM 貼進這個檔案。
2. **上傳伺服器網址、認證金鑰已經可以透過 AP_CONFIG 表單設定**（見第 8.5 節第 23 點），但**伺服器端還沒有任何後端會真的驗證這個認證金鑰**——正式後端需要自己實作檢查 `X-API-Key` 標頭的邏輯，不然這個機制形同虛設。
3. **熱點設定模式的密碼已經改成每台裝置唯一衍生**（見第 8.5 節第 24 點），這條限制已解除。
4. **待傳資料的 flash 持久化已改用 littlefs 解決 wear-leveling 問題**（見第 5.1/8.5 節第 28 點），這條限制已解除，但**還沒有實機燒錄測試過**，見第 7.2 節新增的驗證項目。

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
├── qrcode/                  # Nayuki QR-Code-generator，AP_CONFIG 的 WiFi/設定頁 QR code 用，見第 12.7 節
│   ├── qrcodegen.c
│   └── qrcodegen.h
├── littlefs/                # Vendored littlefs v2.11.3（BSD-3-Clause），見第 5.1 節
│   ├── lfs.c/.h
│   ├── lfs_util.c/.h
│   └── LICENSE.md
├── test_server/             # 本機測試用的簡易上傳伺服器（Python）
└── src/
    ├── main.c
    ├── common.h             # 共用資料型別（device_config_t / vital_record_t / vital_type_t），見第 5 節
    ├── state_machine.c/.h
    ├── mode_ap_config.c/.h
    ├── mode_ble_receive.c/.h
    ├── fora_protocol.c/.h   # 三種 FORA 裝置的協定解析，見第 6 節
    ├── mode_upload.c/.h
    ├── upload_api.c/.h
    ├── wall_clock.c/.h      # NTP 校時，boot-relative ms 換算成真實世界 epoch ms
    ├── led_status.c/.h
    ├── display_status.c/.h # 電子紙顯示器封裝，見第 12 節
    ├── storage.c/.h         # 設定值 + 待傳紀錄 + 已上傳歷史，掛載 littlefs 持久化
    └── lfs_pico_hal.c/.h    # littlefs 的 RP2040 flash block-device 介面層，見第 5.1 節
```

## 11. 里程碑

1. ✅ 環境就緒。
2. ✅ 骨架就緒（狀態機 + LED + BOOTSEL 開機視窗）。
3. ✅ 熱點設定模式：已實機驗證手機連線、captive portal 自動彈出、表單送出、設定值讀回並帶入既有值。
4. ✅ BLE 接收模式：額溫槍、血氧計、血壓計、血糖（同一台 D40 二合一機）四種讀值都已完整實機驗證（血壓計/血糖走反編譯確認的自訂協定，見第 6.3/6.4/8 節），血糖數值也已跟裝置螢幕比對一致。
5. ✅ 上傳模式：WiFi 連線 + HTTPS 上傳已完整驗證多次成功，含多種認證模式重試、DHCP 誤判 bug 修復、NTP 校時、失敗自動重試＋斷電持久化。🔧 伺服器網址/認證金鑰可設定、TLS 憑證驗證框架已接好（見第 8.5 節），還沒有正式後端網址可以完成最後一步。
6. ⬜ 24 小時連續運作測試，觀察 flash 抹寫、記憶體、藍芽/WiFi 穩定度——**交接給驗證工程師執行**。
7. ✅ 電子紙顯示器（Waveshare Pico-ePaper-2.9）Phase 1+2 都已燒錄實機驗證：接線/文字顯示正常，四種模式真實資料畫面都測過，詳見第 12 節。✅ Phase 4 提前做的 AP_CONFIG WiFi QR code（12.7 節）2026-08-06 已實機掃碼驗證，手機會正常跳出「加入 WiFi」提示。~~Phase 3（局部刷新排程）~~已評估後決定不做，見第 12.5 節——沒有實測觀察到的具體場景真的需要它。

## 12. 電子紙顯示器（Waveshare Pico-ePaper-2.9）規劃

> 目前裝置只靠 LED 燈號（見第 4 節）當作使用者能看到的唯一回饋，燈號規則需要背起來才看得懂，個管師/家屬完全看不出裝置在幹嘛。接上這片 296×128 電子紙後，目標是讓螢幕變成「不用懂技術也看得懂」的儀表板，LED 保留當底層、隨時看得到的心跳/錯誤指示（螢幕更新慢，LED 補足即時性），兩者不是取代關係。

### 12.1 硬體

- 型號：**Waveshare Pico-CapTouch-ePaper-2.9**（2026-08-06 上網查證確認的正確型號，之前誤記成無觸控的 `Pico-ePaper-2.9`；SSD1680 控制器，V2 時序），296×128，黑白（4 灰階但目前只用黑白）。
- 直接疊在 Pico W 上當 HAT，電子紙走硬體 **SPI1**：DIN→GP11、CLK→GP10、CS→GP9、DC→GP8、RST→GP12、BUSY→GP13、VCC→VSYS、GND→GND。跟 CYW43（WiFi/藍牙晶片）走的是完全不同的內部接腳，不衝突。
- 這個型號額外帶的硬體（目前程式碼只用了按鍵，觸控完全沒用到）：
  - **電容觸控**（5 點，觸控 IC 走 **I2C1**：SDA→GP6、SCL→GP7、RST→GP16、INT→GP17，I2C 位址 0x48）。**目前完全沒有實作**，見第 12.8 節未來可能的擴充方向。
  - **KEY0/KEY1/KEY2 三顆板載按鍵**（GP2/GP3/GP15，內建上拉、按下接地）。**目前只用了 KEY0/KEY1**，見第 2.2 節、`src/button_input.c/.h`；KEY2（GP15）保留未用。
  - **另有一顆 RUN 按鍵**（2026-08-06 實機照片確認，板子上一排總共 4 顆按鍵：RUN/KEY2/KEY1/KEY0），這顆接的是 RP2040 的 **RUN/RESET 腳位**，不是一般 GPIO——按下去等同重置整顆晶片（效果跟拔插電源一樣），韌體完全無法感知、也不需要寫任何程式碼處理。**（2026-08-28 更新）** 按一下 RUN 就會重開機、自動進 AP_CONFIG（見第 2.2 節），不再需要搭配按住 BOOTSEL——這條組合鍵是改版前用來取代「拔插電源＋按住 BOOTSEL」的做法，現在拔插電源／按 RUN 效果相同，都可以直接用。
- 因為現場可能有沒接這片電子紙的「無螢幕版本」（純 Pico W），按鍵/觸控相關程式碼都設計成硬體不存在時安全無害（讀到的一律是預設值，不會誤觸發），見第 2.2 節的說明。

### 12.2 驅動來源

- 從 Waveshare 官方 `Pico_ePaper_Code` repo（`c/lib/`）複製過來，放在專案根目錄 `epd/`（跟 `dhcpserver.c`／`dnsserver.c` 一樣是「從外部複製進來的第三方程式碼」，不算我們自己維護的邏輯）：
  - `DEV_Config.c/.h`：SPI1 + GPIO 底層存取。**改過兩處**：(1) 原版 `DEV_Module_Init()` 內部會呼叫 `stdio_init_all()`，但 `main.c` 已經呼叫過一次，拿掉重複呼叫。(2) **（2026-08-06 加做，尚未實機測試，見下方「無螢幕相容性」）** `DEV_GPIO_Init()` 幫 `EPD_BUSY_PIN` 加了 `gpio_pull_down()`。
  - `EPD_2in9_V2.c/.h`：SSD1680 指令層，提供 `EPD_2IN9_V2_Init/Clear/Display/Display_Base/Display_Partial/Sleep`。**改過一處（2026-08-06，尚未實機測試）**：`EPD_2IN9_V2_ReadBusy()` 原版是無限迴圈等待 BUSY 腳位變化，加了 5 秒逾時上限，逾時就放棄等待繼續往下執行（見下方「無螢幕相容性」）。
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
| AP_CONFIG 熱點設定 | 熱點 SSID/密碼文字、目前已存的個案編號（ASCII 過濾過，沒設定過就顯示 `(unset)`）、操作提示、**兩個並排 QR code**（WiFi 帳密 + 設定頁網址，各自標註用途，見 12.7 節） | `display_status_show_ap_config()` |
| BLE_RECEIVE | 個案編號（方案 A）、目前狀態（`Scanning (MM/DD HH:MM)`，見下方心跳說明）、體溫/血氧/脈搏/**血糖（協定已實作，見 6.4 節，沒量過的話顯示 `-- (never)`）**/血壓各自最後一筆數值＋時間戳（已校時顯示 `MM/DD HH:MM` 絕對時間，血壓計顯示的是**裝置自己的量測時間**而非 Pico 收到時間，見第 6.3 節；未校時且無裝置時間戳顯示 `unsynced,+Nm`）、**MD6 六合一血糖以外 5 項合併列**（HCT/Ketone/UA/Chol/HB，見下方說明）、**最後一次成功上傳時間**、待上傳筆數 | `display_status_set_ble_receive()` + `display_status_poll()` |

（2026-08-13 版面優化，**已 build 成功，還沒燒錄/肉眼驗證實際排版**：ID 跟狀態文字合併成一行、Last upload 跟 Pending 合併成一行，各省一行螢幕空間；體溫/血氧/脈搏/血糖/血壓五行往上移到 y=22~74，省下來的空間留給 FORA MD6 六合一新增的項目用，見第 6.5 節。）

**2026-08-26 校正（跟 `display_status.c` 核對後更新）**：上面「留給以後 MD6 用」的空間已經實作完成，不再只是預留——`draw_md6_extra_row()` 畫一行**合併列**，顯示 HCT/Ketone/UA/Chol/HB 這 5 項裡「最後更新的那一項」（用 `MD6 <縮寫> <數值><單位> (時間)` 格式，例如 `MD6 HCT 46% (08/26 10:05)`），5 項共用同一行、不是各自一行；沒有任何一項量過時顯示 `MD6   -- (never)`。

**2026-08-31 版面順序調整**：MD6 合併列從最後一列（y=100）搬到 BP 列之後、Last/Pending 列之前（y=87），Last/Pending 改到 y=100（最後一列）——對應說明簡報「日常量測」頁 1-8 的順序。同一天 `pulse_source_tag()` 也擴充成通用的 `reading_source_tag()`：血糖列（Gluc）比照脈搏列的做法，依 `source_kind` 加註來源標籤——血壓計（D40，血糖模式）顯示 `D40 `、MD6 顯示 `MD6 `、Bionime Rightest GM700SB 顯示 `GM `，例如 `Gluc  107mg/dL (MD6 08/26 09:20)`；脈搏列本身的 `BP `/`O2 ` 標籤不變。
| UPLOAD | WiFi SSID + 目前階段/結果文字（`Connecting...`/`Success (N records)`/`Failed, will retry`） | `display_status_show_upload()` |
| 錯誤 | 一句英文錯誤訊息，取代難記的三連閃燈號 | `display_status_show_error()` |

**「Scanning」狀態文字的心跳設計**：故意不用靜態的 `"Scanning..."`——電子紙斷電/當機時畫面會凍結在最後一次刷新的內容，如果狀態文字本身不含時間資訊，使用者沒辦法從畫面分辨「裝置還活著、只是沒掃到裝置」跟「裝置已經當機」。`update_scanning_status()` 組出帶時間戳的狀態文字，並在 `mode_ble_receive_run()` 主迴圈裡每 180 秒（比照 Waveshare 資料手冊建議的刷新間隔下限，見 12.5.1 節）定期重新呼叫一次，讓時間戳在沒有裝置連線活動時依然會前進。**尚未實機視覺驗證**：程式邏輯已審視過、序列埠 log 能間接佐證運作時機，但還沒有真的空出一段裝置都不在旁邊廣播、超過 180 秒的安靜時間，肉眼確認面板上的時間戳有沒有正確前進（見第 7.2 節第 9 點）。

局部刷新排程（減少全刷閃爍/縮短刷新時間）曾經做過又移除了，見上面 12.5 節——沒有實測觀察到的具體場景真的需要它，決定維持只用全刷。

### 12.7 AP_CONFIG 的 QR code（原規劃 Phase 4 提前；2026-08-28 加了第二個 QR code）

- 手機相機掃到 `WIFI:T:WPA;S:<ssid>;P:<password>;;` 這個特定前綴的字串會自動跳出系統內建的「加入 WiFi」提示——這是業界慣例（源自 ZXing），**QR code 本身沒有什麼特殊格式/模式**，跟顯示純文字/網址的 QR code 用的是同一套編碼方式，差別只在字串內容。SSID/密碼裡如果出現 `\`、`;`、`,`、`:`、`"` 這幾個字元，依慣例要加反斜線跳脫（`display_status.c` 的 `append_escaped_wifi_field()`）——`AP_SSID` 是寫死字串、`generate_ap_password()` 衍生出的密碼固定是 `pico-` 加 8 位十六進位字元，兩者都不會出現這些字元，但當初先做完整這件事現在證實是對的：密碼後來（見第 8.5 節第 24 點）真的從寫死字串改成動態產生了，這個跳脫邏輯不用跟著改。
- 這個 QR code 編碼的是**熱點本身**的 SSID/密碼（讓手機能連上 Pico 的設定用熱點），**不是**個案要接的目標 WiFi（`device_config_t.wifi_ssid`/`wifi_password`，那組帳密是使用者在熱點頁面的表單裡填的，不會出現在任何 QR code 上）。**2026-08-26 校正（跟 `mode_ap_config.c` 核對後更新）**：熱點 SSID 仍是 `generate_ap_ssid()` 衍生的每台裝置專屬值（`GATEWAY-XXXX`，見第 8.5 節第 24 點）；但密碼**不再是**動態衍生值——見第 8.5 節第 39 點，因為無螢幕版本沒有任何管道能讓使用者知道衍生出來的密碼，密碼已改回固定值 `02750963`（所有裝置共用同一組），QR code 編的就是這個固定密碼加上該台裝置的 SSID。
- QR code 編碼器：從 Nayuki 的 `QR-Code-generator`（`c/qrcodegen.c`/`.h`，MIT 授權）複製進專案根目錄 `qrcode/`，純 C89、沒有外部依賴（只用標準函式庫），沒有改動任何一行。呼叫 `qrcodegen_encodeText()`，ECC 等級用 MEDIUM（可以容忍約 15% 資料損毀，兼顧掃描容錯率與 QR 大小），版本上限給 10（`qrcodegen_BUFFER_LEN_FOR_VERSION(10)` 對應的 buffer 只有 408 bytes，對這種 40~50 bytes 的短字串綽綽有餘，實際會落在 version 2~3 左右，遠用不到上限）。
- 畫在 AP_CONFIG 畫面右側（`draw_qr_code()`，把 QR 的每個 module 依比例放大成好幾個實際像素畫上去，四周留白靠 `Paint_Clear(WHITE)` 清出來的背景自然滿足，不用額外處理），文字（SSID/密碼/個案編號/操作提示）留在左側，兩者之間留了足夠間距。
- ✅ **2026-08-06 已實機掃碼驗證**，見第 7.2 節第 8 點。

**（2026-08-28 加做，尚未實機測試）第二個 QR code——設定頁網址**：使用者反應只有 WiFi QR code 不夠直覺——手機加入熱點後理論上會自動跳出 captive portal，但不同系統/設定有時候不會跳（尤其部分手機的「智慧網路切換」或使用者手動關掉了彈跳頁提示），這種情況下使用者不知道要自己開瀏覽器打 `192.168.4.1`。加了第二個 QR code，編碼 `http://192.168.4.1/`（`display_status.c` 的 `AP_CONFIG_SETUP_URL` 常數，跟 `mode_ap_config.c` 的 `dhcp_server_init()` 用的 gw IP 是同一個值——這兩處沒有共用同一個常數，一個是 lwIP 的 `ip4_addr_t`、一個是給人看/掃的字串，之後如果改 gw IP 要記得兩邊一起改），手機相機掃到會直接跳出開網址的提示，不需要先加入熱點再自己找網址。
- 兩個 QR code 並排畫在畫面右側：WiFi 帳密在左、設定頁網址在右，各自上方都印一行文字標註是做什麼用的（"Join WiFi:" / "Setup page:"），避免使用者搞不清楚要掃哪一個、掃了會發生什麼事；網址那個下方另外印一行純文字 `192.168.4.1`，QR 掃不了（相機權限、光線不好等）還能用手打。
- `draw_qr_code()` 同時改成把實際畫出來的 QR 內容置中在傳入的方框裡（原本固定貼齊左上角，方框比實際內容大時右下角會留白）——兩個方框並排時，內容置中看起來才會對齊。

**（2026-08-28 第二版版面調整，使用者實機看過畫面後要求）**：
1. 標題從 "WiFi Setup" 改短成 "Setup"。
2. 除了標題以外全部文字統一字級（Font12）——原本 SSID/密碼/兩個 QR 的說明文字是比較窄的 Font8、個案編號是 Font12，粗細大小不一致；改完只有標題（Font16）比其他文字大。
3. SSID/密碼/個案編號原本是「標籤: 值」同一行，Font12 比 Font8 寬，同一行常常放不下（尤其 SSID 帶了 MAC 衍生字尾），改成標籤跟值統一各佔一行。
4. 拿掉左側文字欄下方原本的 "Scan QR below ->" 提示行——兩個 QR 自己上方已經有說明文字，這行變多餘。
5. 兩個 QR code 改成**同樣大小**的方框（`AP_CONFIG_QR_BOX_PX`，88x88px，之前是 96x96/64x64 不一樣大）——WiFi 帳密字串比較長，編出來的 QR 版本（module 數）比較多，同一個方框裡 `draw_qr_code()` 照樣會自動縮小到剛好塞得下，不用個別調整方框大小；缺點是兩個 QR 實際畫出來的清晰度（每個 module 對應的實際像素數）會不一樣——網址短、module 少，同樣的方框裡每個 module 能分到比較多像素，比 WiFi 那個更清楚。
- **尚未實機測試**：只驗證過 `cmake --build` 成功，還沒有肉眼確認電子紙螢幕上的實際排版、也沒有拿手機實際掃過這個新的網址 QR code。

### 12.8 板載按鍵（2026-08-06 加做，尚未實機測試）

見第 2.2 節完整說明。實際硬體是 Pico-CapTouch-ePaper-2.9（見 12.1 節），帶 KEY0/KEY1/KEY2 三顆按鍵跟一組電容觸控 IC。`src/button_input.c/.h` 負責讀取，`mode_ble_receive.c` 的主迴圈輪詢：
- **KEY0**：長按 3 秒進 AP_CONFIG，回傳新增的 `mode_ble_receive_exit_t` 列舉值告知 `state_machine.c` 該切去哪個模式。
- **KEY1**：按一下手動觸發上傳，跳過 idle timeout 等待；同時呼叫 `wall_clock_request_resync()`（2026-08-06 加做，尚未實機測試）強制這次上傳前重新查詢 NTP，不管距離上次成功校時是否還沒超過 `NTP_RESYNC_INTERVAL_MS`（6 小時，見 `wall_clock.c`）——手動觸發時使用者通常也想確認時間校得準不準，不要被例行的省電/省網路節流擋下來。
- **KEY2（2026-08-06 加做；2026-08-28 改成顯示未上傳紀錄+可翻頁）**：顯示未上傳（`PENDING`/`FAILED`）紀錄畫面（`display_status_show_pending_records()`，一次一頁、每頁最多 7 筆，見 `storage_pending_records_page()`），顯示 8 秒後自動換回 BLE_RECEIVE 即時畫面。這是先前討論觸控方案時評估後選的低成本替代方案——不用碰觸控 IC、不用重新引入局部刷新，直接複用現有全刷機制。原本（2026-08-06）顯示的是已上傳歷史（`display_status_show_upload_history()`／`storage_get_recent_upload_history()`），使用者反應看不到還在待傳/上傳失敗的東西不夠用，改成反過來：預設顯示未上傳，畫面還顯示著的時候再按一次 KEY2 會翻到下一頁（`s_history_page`，翻完最後一頁繞回第 0 頁），逾時換回即時畫面之後再按則從第 0 頁重新開始；每筆記錄前面標一個字元區分狀態（`.` 待傳中、`!` 上傳失敗過）。顯示期間 `mode_ble_receive_run()` 主迴圈會暫停呼叫 `display_status_poll()`（不然畫面會馬上被即時內容蓋掉），逾時後恢復輪詢；BLE 掃描/連線本身不受影響，暫停的只有畫面刷新。**尚未實機測試**：只驗證過 `cmake --build` 成功。

**設計上刻意不影響 idle timeout 自動上傳（第 3 節）**：KEY0/KEY1/KEY2 只是新增的觸發路徑，跟原本的路徑並存。GPIO 用軟體內建上拉讀取，沒有接這片電子紙的「無螢幕版本」永遠讀到「沒按下」，不需要任何編譯選項區分兩種硬體。**（2026-08-28 更新）** 原本這裡還提到「BOOTSEL 開機視窗原封不動保留」——這條路徑跟對應的 `mode_boot_select.c/.h` 已經整個移除（見第 2.2 節），開機不再需要按任何按鍵，一律直接進 AP_CONFIG。

**觸控（5 點電容觸控，I2C1 走 GP6/GP7/GP16/GP17，見 12.1 節）目前完全沒有實作，也沒有計畫做**：評估後認為觸控+局部刷新的完整方案（新驅動、新 UI、重新引入局刷）工作量/風險不小，KEY2 陽春文字畫面已經解決核心需求（看得到歷史），觸控維持只是預留的擴充空間。

### 12.9 無螢幕版本相容性（2026-08-06 全盤檢查，尚未實機測試）

因為現場可能有不接電子紙板的「無螢幕版本」（純 Pico W），逐一檢查了所有跟這片板子接觸的程式碼，有沒有假設「面板一定存在」。

**找到一個嚴重問題並已修復**：`epd/EPD_2in9_V2.c` 的 `EPD_2IN9_V2_ReadBusy()`（Waveshare 原版函式）是**無限迴圈**，一直讀 `EPD_BUSY_PIN`（GP13）直到讀到 0 才返回，沒有任何逾時。這個函式在 `EPD_2IN9_V2_Init()` 一開始就會被呼叫，而 `display_status.c` 的第一次畫面刷新（`end_frame_and_refresh()` → `EPD_2IN9_V2_Init()`）在開機後幾乎立刻就會發生，不管是進 AP_CONFIG 還是 BLE_RECEIVE 都一樣。原本的 `DEV_Config.c` 把 `EPD_BUSY_PIN` 設成沒有內建上拉/下拉的浮接輸入——**沒有實際接上電子紙板時，這個腳位電位是未定義的，讀到的值不保證會變成 0**，代表無螢幕版本的機器極可能在開機後的第一次畫面刷新就永久卡死在這個無限迴圈裡，BLE 接收、WiFi 上傳、AP_CONFIG 全部都會停擺，完全違背「無螢幕版本」的初衷。

**修法（兩處，互相搭配）**：
1. `DEV_Config.c` 的 `DEV_GPIO_Init()` 幫 `EPD_BUSY_PIN` 加 `gpio_pull_down()`——沒有面板時這個腳位會穩定讀到 0（不忙），`ReadBusy()` 第一次檢查就會直接返回，不需要真的等到逾時。面板真的接上時，SSD1680 控制器會主動驅動這個腳位（推挽輸出），電位由控制器決定，會蓋過 RP2040 這邊微弱的內建下拉電阻，不影響有面板時的正常行為。
2. `EPD_2in9_V2.c` 的 `EPD_2IN9_V2_ReadBusy()` 加了 5 秒逾時上限，逾時就放棄等待、繼續往下執行——這是保底機制，防的是「面板真的接上但故障/異常」這種第 1 點解決不了的情況，5 秒遠大於 SSD1680 資料手冊給的正常等待時間，不會誤傷有面板時的正常操作。

**其餘檢查過、確認沒問題的部分**：
- `EPD_2IN9_V2_Sleep()`、SPI 寫入本身（`spi_write_blocking()`）、`GUI_Paint.c` 的繪圖函式都不依賴外部腳位電位，沒有無限迴圈風險（已用 `grep` 掃過整個 `epd/`／`qrcode/`／`dhcpserver.c`／`dnsserver.c`，確認只有這一處無限迴圈）。
- KEY0/KEY1（`button_input.c`）本來就用內建上拉讀取，沒接電子紙板時穩定讀到「沒按下」，見第 12.8 節。
- `display_status_show_xxx()`／`display_status_poll()` 都是 `void`，呼叫端（`mode_ap_config.c`／`mode_ble_receive.c`）不會因為畫面刷新失敗/逾時而卡住或走錯分支，跟畫面顯示完全解耦。

**還沒驗證**：這整組修法目前只確認編譯過關，還沒有實機測試過「真的不接電子紙板開機」是否能正常運作到 BLE_RECEIVE／AP_CONFIG，也還沒測過「面板正常接上時，這兩處改動有沒有意外拖慢或影響正常刷新」。
