#ifndef LWIPOPTS_H
#define LWIPOPTS_H

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
#endif

#endif // LWIPOPTS_H
