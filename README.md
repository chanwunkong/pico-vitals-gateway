# pico — Raspberry Pi Pico W 生理訊號中繼裝置

專案規劃與目前進度見 [PROJECT_PLAN.md](PROJECT_PLAN.md)。

## 開發環境（已裝好）

本機已安裝並驗證可編譯燒錄：

- pico-sdk v2.3.0（clone 在 `C:\Users\307\pico-sdk`，`PICO_SDK_PATH` 已設成使用者環境變數）
- ARM GNU Toolchain 14.2、CMake、Ninja（透過 winget 安裝）
- MinGW-w64（WinLibs，供 host 端編譯 pioasm/picotool 用）
- VSCode 官方 "Raspberry Pi Pico" 擴充套件（也提供了自己的一份 SDK/工具鏈於 `~/.pico-sdk`，內含含 USB 支援的 `picotool`，燒錄時優先用這份）

編譯：

```
cmake -S . -B build -G Ninja
cmake --build build
```

燒錄（裝置目前若已在跑韌體，用 picotool 直接觸發重開機進 BOOTSEL 再複製 `.uf2`）：

```
"C:\Users\307\.pico-sdk\picotool\2.3.0\picotool\picotool.exe" reboot -u -f
# 等 D:\ 出現 RPI-RP2 磁碟機後
copy build\pico_gateway.uf2 D:\
```

除錯：裝 VSCode 的 **Serial Monitor** 擴充套件（`ms-vscode.vscode-serial-monitor`），`Ctrl+Shift+P` → `Serial Monitor: Start Monitoring` → 選裝置的 COM port、baud rate 115200，即可即時看到韌體的 log。

## 專案現況

- 狀態機（開機 BOOTSEL 視窗 → 熱點設定 / BLE 接收 / 上傳）、LED 燈號、flash/RAM 儲存已建立並在實機驗證正常運作。
- **BLE 接收模式已完整打通**，實測能連線 FORA IR42 額溫槍並正確解析出體溫數值。真實協定跟一開始從官方標準文件假設的完全不同，細節見 [PROJECT_PLAN.md](PROJECT_PLAN.md) 第 7.1 節。
- `src/upload_api.c` 仍是 TODO stub，等實際上傳 API 規格確定後再實作。
- `src/mode_ap_config.c` 已寫好但尚未實機驗證，需要的 `dhcpserver.c`/`dnsserver.c` 已放在專案根目錄（複製自 pico-examples）。
- 已知待辦：FORA IR42 量測後會持續廣播一段時間，目前每次掃到都會重新連線觸發，同一次量測可能被重複記錄，見 PROJECT_PLAN.md 第 7 節第 8 點。
