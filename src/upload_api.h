#ifndef UPLOAD_API_H
#define UPLOAD_API_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>

// 將一批待傳紀錄上傳到指定 API，回傳是否成功（呼叫端會依此標記紀錄狀態）。
// 測試階段：純 HTTP（非 HTTPS），伺服器位址寫死在 upload_api.c；
// 正式對接時要換成真實 endpoint、認證方式，並讓伺服器位址可設定。
bool upload_api_post_batch(const char *patient_id, const vital_record_t *records, size_t count);

#endif // UPLOAD_API_H
