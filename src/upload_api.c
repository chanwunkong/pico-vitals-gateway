#include "upload_api.h"

bool upload_api_post_batch(const vital_record_t *records, size_t count) {
    (void)records;
    (void)count;
    // TODO: 實作 HTTP POST 上傳。骨架階段先回傳 false，讓 storage 保留紀錄等待下次重試。
    return false;
}
