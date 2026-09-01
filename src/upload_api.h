#ifndef UPLOAD_API_H
#define UPLOAD_API_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 把這批待傳紀錄彙整成一筆後端要的綜合生理量測紀錄（見 upload_api.c 開頭的
// 說明）透過 HTTP 上傳到指定 API，回傳是否成功（呼叫端會依此標記紀錄狀態）。
// server_host/api_key 來自 device_config_t（AP_CONFIG 表單設定），留空（NULL
// 或 ""）的話退回 upload_api.c 內建的測試預設主機、不加認證標頭。
// upload_time_ms 是要送給後端 uploadTime 欄位的真實世界 epoch ms——2026-08-31
// 起不再是「上傳當下」，改成呼叫端先用 fora_protocol_resolve_epoch_ms() 依
// 跟畫面顯示同一套規則算出來的時間（優先用裝置自己的量測時間，沒有裝置時間
// 戳或裝置時鐘不合理才退回中繼器收到當下的時間），這裡只負責原樣送出，不
// 重新計算。wall_clock_synced 為 false 時代表這個時間戳不可信，整個 uploadTime
// 欄位會被省略、不會謊報一個假的時間戳。source_kind 是這批 records 的來源裝置種類（fora_device_kind_t，
// 這裡用不透明的 uint8_t 存，理由跟 common.h vital_record_t.source_kind 一樣
// 避免循環 include）——records 裡每一筆都必須是這個種類，呼叫端負責先依裝置
// 種類分組（見 upload_api.c 的說明），才能讓後端的 dataSource 正確標示是哪種
// 醫材量的。
bool upload_api_post_batch(const char *patient_id, const char *server_host, const char *api_key,
                            const vital_record_t *records, size_t count,
                            uint64_t upload_time_ms, bool wall_clock_synced, uint8_t source_kind);

#endif // UPLOAD_API_H
