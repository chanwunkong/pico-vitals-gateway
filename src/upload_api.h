#ifndef UPLOAD_API_H
#define UPLOAD_API_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>

// 將一批待傳紀錄上傳到指定 API，回傳是否成功（呼叫端會依此標記紀錄狀態）。
// TODO: 換成實際 API endpoint、認證方式、JSON payload 格式；建議用 lwIP altcp API
// 手刻 HTTP POST（若需要 HTTPS 再加 pico_lwip_mbedtls / pico_mbedtls）。
bool upload_api_post_batch(const vital_record_t *records, size_t count);

#endif // UPLOAD_API_H
