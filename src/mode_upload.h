#ifndef MODE_UPLOAD_H
#define MODE_UPLOAD_H

// 上傳模式：連線 WiFi、嘗試上傳所有待傳生理資料。
// 無論成功或失敗都會返回——呼叫端（state_machine）固定轉回 BLE 接收模式。
void mode_upload_run(void);

#endif // MODE_UPLOAD_H
