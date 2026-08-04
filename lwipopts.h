#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include <stdint.h>

// 改編自 pico-examples 各 pico_w wifi 範例共用的 lwipopts.h 樣板（NO_SYS=1，
// 搭配 pico_cyw43_arch_lwip_threadsafe_background）。實際編譯若 lwIP 抱怨缺少
// 巨集，請對照 pico-examples/pico_w/wifi/*/lwipopts.h 調整。

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1

// 開機時沒有網路，沒辦法知道真實時間；只有上傳那一刻 WiFi 連上時才有機會問
// 網路上的 NTP 伺服器校時，把量測時的 boot-relative ms 換算成真實世界時間，
// 見 wall_clock.c。SNTP_SET_SYSTEM_TIME_US 收到 NTP 回應時被 lwIP 呼叫，
// wall_clock_sntp_set_system_time_us() 定義在 wall_clock.c。
#define SNTP_SERVER_DNS             1
#ifdef __cplusplus
extern "C" {
#endif
void wall_clock_sntp_set_system_time_us(uint32_t sec, uint32_t us);
#ifdef __cplusplus
}
#endif
#define SNTP_SET_SYSTEM_TIME_US(sec, us) wall_clock_sntp_set_system_time_us(sec, us)
// lwIP sntp.c 的註解要求用 SNTP 就要多留一個 timeout 插槽。
#define MEMP_NUM_SYS_TIMEOUT        12

// 上傳功能要打公開網際網路上的 https 網址（見 upload_api.c），需要 altcp + TLS。
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
#define LWIP_DHCP_DOES_ARP_CHECK    0
#define LWIP_DHCP_DOESNT_CALL_NETIF_SET_UP 1

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_TCP_KEEPALIVE          1

#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0

#define TCPIP_MBOX_SIZE             8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8

#define ETH_PAD_SIZE                0
#define LWIP_CHKSUM_ALGORITHM       3

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
// 需要重新診斷 DHCP/WiFi 連線問題時，把這兩行取消註解（注意：這個 project
// 目前是 Release build 會定義 NDEBUG，所以要暫時拿掉 NDEBUG 或搬到這個
// #ifndef 區塊外面才會真的生效）。
// #define DHCP_DEBUG                  LWIP_DBG_ON
// #define NETIF_DEBUG                 LWIP_DBG_ON
#endif

#endif // LWIPOPTS_H
