#include "fora_protocol.h"

#include "btstack.h"

#include <string.h>

const uint8_t FORA_SERVICE_UUID128[16] = FORA_SERVICE_UUID128_INIT;
const uint8_t FORA_CHARACTERISTIC_UUID128[16] = FORA_CHARACTERISTIC_UUID128_INIT;
const uint8_t FORA_TRIGGER_COMMAND[8] = { 0x51, 0x26, 0x00, 0x00, 0x00, 0x00, 0xa3, 0x1a };

bool fora_protocol_matches_advertisement(const uint8_t *adv_data, uint8_t adv_len) {
    ad_context_t context;
    for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        uint8_t data_len = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);

        if (data_type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME &&
            data_type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
            continue;
        }

        // 子字串比對裝置名稱是否包含 "FORA"，不用完全比對，避免不同型號/序號
        // 尾綴造成比對失敗。裝置名稱可能只出現在 scan response 封包裡。
        for (uint8_t i = 0; i + 4 <= data_len; i++) {
            if (memcmp(&data[i], "FORA", 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool fora_protocol_parse_reading(const uint8_t *value, uint16_t len, vital_record_t *out) {
    if (len < 4 || value[0] != 0x51) {
        return false; // 不是我們觸發指令的回應封包
    }

    uint16_t raw = (((uint16_t)value[3] << 8) | value[2]) & 0x0FFF;

    out->type = VITAL_TYPE_TEMPERATURE;
    out->value = (float)raw / 10.0f;
    return true;
}
