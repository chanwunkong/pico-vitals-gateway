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
    src/storage.c
    src/wall_clock.c
    dhcpserver.c
    dnsserver.c
)

target_include_directories(pico_gateway PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
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
    pico_flash
)

pico_add_extra_outputs(pico_gateway)
