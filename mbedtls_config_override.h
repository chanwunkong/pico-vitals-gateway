#ifndef PICO_VITALS_MBEDTLS_CONFIG_OVERRIDE_H
#define PICO_VITALS_MBEDTLS_CONFIG_OVERRIDE_H

// RP2040 只有 264KB RAM，要跟 BTstack/lwIP 共存，mbedtls 預設的 TLS record buffer
// （收/送各 16KB）太大，縮小成 4KB。TLS 協定允許把交握訊息拆成多個 record 傳送，
// 縮小 buffer 不影響跟伺服器端的相容性（Cloudflare 邊緣節點等一般 TLS 伺服器都支援）。
// pico-sdk 內建的 lwIP altcp_tls glue code（altcp_tls_mbedtls.c）會直接存取
// mbedtls_ssl_context 的內部欄位（例如 out_left）。mbedtls 3.x 預設把這些欄位
// 改名隱藏（要用 MBEDTLS_PRIVATE() 包），這裡開放存取，這是 mbedtls 官方提供
// 給需要碰內部欄位的舊程式碼用的標準開關，不是繞過安全機制。
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "mbedtls/mbedtls_config.h"

// mbedtls 預設的亂數來源（entropy_poll.c）只支援 Unix/Windows，bare-metal ARM
// 編譯會直接 #error。關掉那段平台專屬程式碼，改用 pico-sdk 提供的硬體亂數
// （RP2040 的 ROSC-based get_rand_64()，見 pico_mbedtls.c 的
// mbedtls_hardware_poll() 實作）。兩個巨集都要開，缺一不可：
// NO_PLATFORM_ENTROPY 關掉編不過的 OS 專屬程式碼，ENTROPY_HARDWARE_ALT 才是
// 真正接上 pico-sdk 硬體亂數來源的開關。
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

// 同理，mbedtls_ms_time() 預設實作只認 POSIX/Windows，bare-metal 一樣編不過。
// 改用我們自己在 upload_api.c 提供的版本（包住 pico SDK 的單調時鐘）。
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// timing.c（MBEDTLS_TIMING_C）也是只認 Unix/Windows 的模組，主要給 DTLS
// 重傳計時器跟 self-test 用，我們只走一般 TCP+TLS，不需要，直接關掉。
#undef MBEDTLS_TIMING_C

// MBEDTLS_FS_IO 讓 x509_crt.c 多編出從檔案系統路徑載入憑證的功能（用
// opendir/readdir），bare-metal 沒有檔案系統，也編不過。我們的憑證/CA 一律
// 用記憶體裡的 byte array 傳（見 altcp_tls_create_config_client 的用法），
// 不需要這個功能。
#undef MBEDTLS_FS_IO

// mbedtls 內建的 net_sockets.c（MBEDTLS_NET_C）是它自己的 BSD socket
// 傳輸層封裝，一樣只認 Unix/Windows。我們不用它——傳輸層是透過 lwIP 的
// altcp_tls 串接，跟這個模組無關，直接關掉。
#undef MBEDTLS_NET_C

// PSA 的「Internal Trusted Storage」檔案版實作依賴 MBEDTLS_FS_IO，關掉
// FS_IO 之後這個也要跟著關，不然 check_config.h 會抱怨前置條件不齊。
#undef MBEDTLS_PSA_ITS_FILE_C

// PSA crypto 的持久化金鑰儲存層（psa_crypto_storage.c）預設也開著，一樣需要
// 檔案系統（psa/error.h 等 PSA 標頭在這個 bare-metal 編譯環境沒有產生）。
// 我們沒有用到 PSA 持久化金鑰（MBEDTLS_USE_PSA_CRYPTO 本來就沒開），關掉。
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

// 整個 PSA crypto 層直接關掉：pico-sdk 的 CMakeLists 編譯清單漏收了
// psa_crypto_random.c（PSA 這層要用的隨機數 glue code），導致連結時一堆
// psa_random_internal_* 符號找不到（pico-sdk 本身的疏漏，不是我們設定錯）。
// 因為 TLS 交握走的是舊版 mbedtls_xxx 加密 API（MBEDTLS_USE_PSA_CRYPTO 本來
// 就沒開），PSA 這層對我們來說整個是多餘的，關掉最乾淨。
#undef MBEDTLS_PSA_CRYPTO_C

// 關掉 PSA crypto 之後，兩個預設開著、但需要 PSA_CRYPTO_C 當前置條件的功能
// 也要跟著關，不然 check_config.h 會報前置條件不齊：
// - MBEDTLS_LMS_C：一種簽章演算法，我們用不到。
// - TLS 1.3：mbedtls 這個版本的 TLS 1.3 金鑰推導需要 PSA_CRYPTO_C。我們只需要
//   跟 Cloudflare 邊緣節點做基本的 HTTPS POST，TLS 1.2（保留開著）完全足夠。
#undef MBEDTLS_LMS_C
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED

#undef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN  4096

#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif // PICO_VITALS_MBEDTLS_CONFIG_OVERRIDE_H
