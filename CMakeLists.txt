cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Board type")

# 上傳功能要用 TLS 打 Cloudflare Tunnel 的 https 網址，縮小 mbedtls 預設的 TLS
# buffer 大小以配合 RP2040 有限的 RAM，見 mbedtls_config_override.h 註解。
set(PICO_MBEDTLS_CONFIG_FILE "mbedtls_config_override.h")

include(pico_sdk_import.cmake)

project(pico_gateway C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_gateway
    src/main.c
    src/state_machine.c
    src/mode_boot_select.c
    src/mode_ap_config.c
    src/mode_ble_receive.c
    src/fora_protocol.c
    src/mode_upload.c
    src/upload_api.c
    src/led_status.c
    src/button_input.c
    src/display_status.c
    src/storage.c
    src/lfs_pico_hal.c
    src/wall_clock.c
    dhcpserver.c
    dnsserver.c
    # Waveshare Pico-ePaper-2.9 驅動，從 Pico_ePaper_Code 複製，見 PROJECT_PLAN.md 第 12.2 節。
    epd/DEV_Config.c
    epd/EPD_2in9_V2.c
    epd/GUI_Paint.c
    epd/Fonts/font8.c
    epd/Fonts/font12.c
    epd/Fonts/font16.c
    epd/Fonts/font20.c
    epd/Fonts/font24.c
    # QR code 編碼器（Nayuki QR-Code-generator），AP_CONFIG 畫面的 WiFi QR code 用，
    # 見 PROJECT_PLAN.md 第 12.7 節。
    qrcode/qrcodegen.c
    # littlefs（設定值/待傳資料/已上傳歷史的 flash 持久化，取代舊版手刻的固定
    # sector 讀寫，有 wear-leveling），見 PROJECT_PLAN.md 第 9 節、src/storage.c。
    littlefs/lfs.c
    littlefs/lfs_util.c
)

target_include_directories(pico_gateway PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/epd
    ${CMAKE_CURRENT_LIST_DIR}/epd/Fonts
    ${CMAKE_CURRENT_LIST_DIR}/qrcode
    ${CMAKE_CURRENT_LIST_DIR}/littlefs
)

pico_enable_stdio_usb(pico_gateway 1)
pico_enable_stdio_uart(pico_gateway 0)

target_link_libraries(pico_gateway
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_btstack_ble
    pico_btstack_cyw43
    pico_mbedtls
    pico_lwip_mbedtls
    pico_lwip_sntp
    hardware_flash
    hardware_spi
    pico_flash
)

pico_add_extra_outputs(pico_gateway)
