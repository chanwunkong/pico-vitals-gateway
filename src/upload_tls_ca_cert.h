#ifndef UPLOAD_TLS_CA_CERT_H
#define UPLOAD_TLS_CA_CERT_H

// 正式上線前必須處理的項目（見 PROJECT_PLAN.md 第 9 節）：目前沒有嵌入真正
// 的伺服器 CA 憑證，upload_api.c 呼叫 altcp_tls_create_config_client() 時傳
// NULL/0，mbedtls/lwIP 這時候預設驗證模式是 MBEDTLS_SSL_VERIFY_OPTIONAL，
// 等同完全不驗證憑證鏈——握手會照常完成，就算是中間人偽造的憑證也會被接受，
// 有安全風險，只適合開發測試階段。
//
// 這裡先把插槽/流程接好：等正式後端網址確定之後，把該伺服器的憑證（或其
// 上一層 CA 憑證）PEM 內容貼進下面的 UPLOAD_CA_CERT_PEM，upload_api.c 會自動
// 把這個陣列連同長度一起交給 altcp_tls_create_config_client()——lwIP 的
// altcp_tls 封裝只要收到非空的 CA 內容就會自動把驗證模式改成
// MBEDTLS_SSL_VERIFY_REQUIRED（沒帶 CA 才會退回 OPTIONAL），不需要另外呼叫
// 任何 API、也不用改 upload_api.c 的程式碼，只要填好這個檔案就好。
//
// 取得憑證 PEM 的方式（以正式後端網址 your-server.example.com 為例，在任何
// 裝了 openssl 的電腦上執行，不需要在 Pico 上做）：
//   openssl s_client -connect your-server.example.com:443 -showcerts </dev/null \
//     | openssl x509 -outform PEM
// 如果伺服器憑證本身會換發（例如用 Let's Encrypt 自動續期），建議貼上一層
// 簽發者的 CA 憑證（例如 ISRG Root X1），而不是貼站台本身的憑證，這樣換證
// 之後不用跟著改這個檔案重新燒錄韌體。
//
// 貼上去的內容格式範例（保留 BEGIN/END 那兩行，整段當成一個 C 字串字面值）：
//   static const char UPLOAD_CA_CERT_PEM[] =
//       "-----BEGIN CERTIFICATE-----\n"
//       "MIIF.....（省略）.....AAA=\n"
//       "-----END CERTIFICATE-----\n";
//
// 目前是空字串 = 沒有嵌入憑證，upload_api.c 會退回目前的「不驗證」行為，並且
// 每次上傳都會在序列埠 log 印出明顯的警告，不會靜默地維持在不安全的狀態。
static const char UPLOAD_CA_CERT_PEM[] = "";

#endif // UPLOAD_TLS_CA_CERT_H
