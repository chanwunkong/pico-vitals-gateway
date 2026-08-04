#include "fora_protocol.h"

#include "btstack.h"

#include <string.h>

const uint8_t FORA_SERVICE_UUID128[16] = FORA_SERVICE_UUID128_INIT;
const uint8_t FORA_CHARACTERISTIC_UUID128[16] = FORA_CHARACTERISTIC_UUID128_INIT;
const uint8_t FORA_TRIGGER_COMMAND[8] = { 0x51, 0x26, 0x00, 0x00, 0x00, 0x00, 0xa3, 0x1a };

bool fora_protocol_matches_advertisement(
    const uint8_t *adv_data, uint8_t adv_len, fora_device_kind_t *out_kind) {
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
                if (out_kind != NULL) {
                    // 血氧計實測名稱是 "FORA O2"、血壓計是 "FORA D40"；
                    // 額溫槍(IR42)沒有這些字元。
                    bool is_oximeter = false;
                    for (uint8_t j = 0; j + 2 <= data_len; j++) {
                        if (data[j] == 'O' && data[j + 1] == '2') {
                            is_oximeter = true;
                            break;
                        }
                    }
                    bool is_blood_pressure = false;
                    for (uint8_t j = 0; j + 3 <= data_len; j++) {
                        if (data[j] == 'D' && data[j + 1] == '4' && data[j + 2] == '0') {
                            is_blood_pressure = true;
                            break;
                        }
                    }
                    *out_kind = is_oximeter ? FORA_DEVICE_OXIMETER
                              : is_blood_pressure ? FORA_DEVICE_BLOOD_PRESSURE
                              : FORA_DEVICE_THERMOMETER;
                }
                return true;
            }
        }
    }
    return false;
}

// IEEE-11073 SFLOAT：16 bit = 4-bit 有號指數 + 12-bit 有號 mantissa（皆為
// 二補數）。Bluetooth SIG 標準健康量測特徵值（血壓、體溫等）數值都用這個格式。
static float decode_sfloat(uint16_t raw) {
    int16_t mantissa = (int16_t)(raw & 0x0FFF);
    if (mantissa >= 0x0800) {
        mantissa -= 0x1000; // 12-bit 二補數轉回有號值
    }
    int8_t exponent = (int8_t)(raw >> 12);
    if (exponent >= 0x08) {
        exponent -= 0x10; // 4-bit 二補數轉回有號值
    }

    float result = (float)mantissa;
    while (exponent > 0) {
        result *= 10.0f;
        exponent--;
    }
    while (exponent < 0) {
        result /= 10.0f;
        exponent++;
    }
    return result;
}

size_t fora_protocol_parse_reading(
    fora_device_kind_t kind, const uint8_t *value, uint16_t len,
    vital_record_t out[FORA_MAX_READINGS_PER_NOTIFICATION]) {
    if (kind == FORA_DEVICE_BLOOD_PRESSURE) {
        // 標準 Blood Pressure Measurement (0x2A35) 格式，見 fora_protocol.h
        // 開頭的說明；跟其他兩種裝置的自訂 0x51 開頭封包無關。
        if (len < 7) {
            return 0;
        }
        uint8_t flags = value[0];
        bool has_timestamp = (flags & 0x02) != 0;
        bool has_pulse = (flags & 0x04) != 0;

        size_t count = 0;
        uint16_t systolic_raw = (uint16_t)value[1] | ((uint16_t)value[2] << 8);
        uint16_t diastolic_raw = (uint16_t)value[3] | ((uint16_t)value[4] << 8);
        out[count].type = VITAL_TYPE_SYSTOLIC;
        out[count].value = decode_sfloat(systolic_raw);
        count++;
        out[count].type = VITAL_TYPE_DIASTOLIC;
        out[count].value = decode_sfloat(diastolic_raw);
        count++;
        // byte[5..6] 是 mean arterial pressure，目前不需要，不取。

        if (has_pulse) {
            size_t offset = 7;
            if (has_timestamp) {
                offset += 7; // time stamp 固定 7 bytes
            }
            if (len >= offset + 2 && count < FORA_MAX_READINGS_PER_NOTIFICATION) {
                uint16_t pulse_raw = (uint16_t)value[offset] | ((uint16_t)value[offset + 1] << 8);
                out[count].type = VITAL_TYPE_PULSE_RATE;
                out[count].value = decode_sfloat(pulse_raw);
                count++;
            }
        }
        return count;
    }

    if (len < 4 || value[0] != 0x51) {
        return 0; // 不是我們觸發指令的回應封包
    }

    if (kind == FORA_DEVICE_OXIMETER) {
        // 2026-08-03 實測回推（手指夾著、螢幕顯示 SpO2 97% / BPM 71-80 時
        // 收到 51 26 61 00 3c 4c a5 05）：byte[4]/byte[6]/byte[7] 目前不知道
        // 用途，先忽略。
        if (len < 6) {
            return 0;
        }
        uint16_t spo2_raw = (((uint16_t)value[3] << 8) | value[2]) & 0x0FFF;
        out[0].type = VITAL_TYPE_SPO2;
        out[0].value = (float)spo2_raw;
        out[1].type = VITAL_TYPE_PULSE_RATE;
        out[1].value = (float)value[5];
        return 2;
    }

    uint16_t raw = (((uint16_t)value[3] << 8) | value[2]) & 0x0FFF;

    out[0].type = VITAL_TYPE_TEMPERATURE;
    out[0].value = (float)raw / 10.0f;
    return 1;
}
