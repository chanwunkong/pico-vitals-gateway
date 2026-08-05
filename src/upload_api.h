#ifndef UPLOAD_API_H
#define UPLOAD_API_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>

// 將一批待傳紀錄透過 HTTPS 上傳到指定 API，回傳是否成功（呼叫端會依此標記
// 紀錄狀態）。server_host/api_key 來自 device_config_t（AP_CONFIG 表單設定），
// 留空（NULL 或 ""）的話分別退回 upload_api.c 內建的測試預設主機、不加認證
// 標頭。
bool upload_api_post_batch(const char *patient_id, const char *server_host, const char *api_key,
                            const vital_record_t *records, size_t count);

#endif // UPLOAD_API_H
