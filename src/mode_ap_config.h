#ifndef MODE_AP_CONFIG_H
#define MODE_AP_CONFIG_H

// 熱點設定模式：開啟 WiFi 熱點、提供簡易網頁表單讓手機設定 WiFi 帳密與個案資訊。
// 阻塞直到收到表單送出並成功寫入 storage 才返回。
void mode_ap_config_run(void);

#endif // MODE_AP_CONFIG_H
