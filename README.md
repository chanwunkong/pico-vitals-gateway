# pico-vitals-gateway — Raspberry Pi Pico W 生理訊號中繼裝置

專案規劃、目前進度、支援裝置協定細節、**測試交接事項**見 [PROJECT_PLAN.md](PROJECT_PLAN.md)。逐檔案程式碼導覽見 [FIRMWARE_FILES.md](FIRMWARE_FILES.md)。

GitHub（private）：https://github.com/chanwunkong/pico-vitals-gateway

## 開發環境

- pico-sdk v2.3.0（`PICO_SDK_PATH` 設成使用者環境變數，指向 clone 的路徑）
- ARM GNU Toolchain 14.2、CMake、Ninja（透過 winget 安裝）
- MinGW-w64（WinLibs，供 host 端編譯 pioasm/picotool 用——**這個常被漏掉**，漏了會導致編譯最後一步轉 `.uf2` 失敗）

詳細安裝步驟、PowerShell 環境變數的已知問題，見 [PROJECT_PLAN.md](PROJECT_PLAN.md) 第 0 節。

編譯：

```
cmake -S . -B build -G Ninja
cmake --build build
```

燒錄：**這台機器本機建置的 `picotool.exe` 沒有 `libusb-1.0.dll`，遠端觸發重開機（`picotool reboot -u -f`）用不了。** 改用手動方式：按住 BOOTSEL 讓裝置變成 `RPI-RP2` USB 隨身碟，再把 `.uf2` 複製過去：

```
copy build\pico_gateway.uf2 <RPI-RP2磁碟機>:\
```

除錯：裝置會出現一個 USB CDC 序列埠，baud rate 115200，可用 VSCode 的 **Serial Monitor** 擴充套件或任何序列埠工具查看即時 log。

## 專案現況

三種 FORA 裝置（額溫槍、血氧計、血壓計）的連線與協定解析、WiFi 上傳、熱點設定模式都已實作並大致驗證過；血壓計的量測資料解析、24 小時等級的耐用性測試還沒做。**完整現況、已知問題、測試交接清單請看 [PROJECT_PLAN.md](PROJECT_PLAN.md)**，不要只看這份 README。
