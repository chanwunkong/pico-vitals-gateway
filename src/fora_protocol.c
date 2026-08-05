#include "fora_protocol.h"

#include "btstack.h"

#include <string.h>

const uint8_t FORA_SERVICE_UUID128[16] = FORA_SERVICE_UUID128_INIT;
const uint8_t FORA_CHARACTERISTIC_UUID128[16] = FORA_CHARACTERISTIC_UUID128_INIT;
const uint8_t FORA_TRIGGER_COMMAND[8] = { 0x51, 0x26, 0x00, 0x00, 0x00, 0x00, 0xa3, 0x1a };

void fora_protocol_build_command(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint8_t out[8]) {
    out[0] = 0x51;
    out[1] = cmd;
    out[2] = p1;
    out[3] = p2;
    out[4] = p3;
    out[5] = p4;
    out[6] = 0xA3;
    uint32_t sum = 0;
    for (int i = 0; i < 7; i++) {
        sum += out[i];
    }
    out[7] = (uint8_t)(sum & 0xFF);
}

uint32_t fora_protocol_decode_measured_key(const uint8_t record[8]) {
    // 欄位佈局見 fora_protocol.h 開頭註解：
    //   byte[0] = day(bits0-4) | month 低 3 bit(bits5-7)
    //   byte[1] = month 最高 1 bit(bit0) | year_offset(bits1-7，year=+2000)
    //   byte[2] = minute(bits0-5) | 心律不整旗標(bit6)
    //   byte[3] = hour(bits0-4) | IHB 狀態(bits5-6) | 是否為平均值(bit7)
    // 只取 day/month/year/hour/minute（分鐘解析度）組成一個比較用的唯一值，
    // 不需要換算成真正的 epoch time——用途只是拿來判斷「跟上一筆比對是不是
    // 同一個時間戳」，同一分鐘裝置不可能量出兩筆不同記錄（充放氣量測本身就要
    // 30-45 秒），足夠當唯一鍵。刻意不含心律不整/IHB/平均值旗標，這幾個純粹是
    // 附加狀態，不影響「是不是同一次量測」的判斷。
    uint8_t day = record[0] & 0x1F;
    uint8_t month = ((record[0] >> 5) & 0x07) | ((record[1] & 0x01) << 3);
    uint8_t year_offset = record[1] >> 1;
    uint8_t minute = record[2] & 0x3F;
    uint8_t hour = record[3] & 0x1F;
    return ((uint32_t)year_offset << 20) | ((uint32_t)month << 16) |
           ((uint32_t)day << 11) | ((uint32_t)hour << 6) | minute;
}

void fora_protocol_measured_key_to_datetime(
    uint32_t key, unsigned *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute) {
    *minute = key & 0x3F;
    *hour = (key >> 6) & 0x1F;
    *day = (key >> 11) & 0x1F;
    *month = (key >> 16) & 0x0F;
    *year = 2000 + ((key >> 20) & 0x7F);
}

// Howard Hinnant 的 days_from_civil（CC0 授權），civil_from_days 的反函式，
// 跟 display_status.c 的 civil_month_day_from_days() 是同一套演算法的另一半，
// 這裡需要的是完整年/月/日換算成天數，不是隻取月/日。
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    unsigned doy = (153 * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + d - 1; // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t fora_protocol_measured_key_to_epoch_ms(uint32_t key) {
    unsigned year, month, day, hour, minute;
    fora_protocol_measured_key_to_datetime(key, &year, &month, &day, &hour, &minute);
    int64_t days = days_from_civil((int64_t)year, month, day);
    int64_t local_sec = days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60;
    int64_t utc_sec = local_sec - LOCAL_UTC_OFFSET_SEC;
    return (uint64_t)(utc_sec * 1000);
}

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

size_t fora_protocol_parse_reading(
    fora_device_kind_t kind, const uint8_t *value, uint16_t len,
    vital_record_t out[FORA_MAX_READINGS_PER_NOTIFICATION]) {
    if (kind == FORA_DEVICE_BLOOD_PRESSURE) {
        // 呼叫端已經把兩次指令（FORA_BP_CMD_GET_RECORD_PART_A/B）的回應接成
        // 8 bytes 了，這裡直接照 fora_protocol.h 說明的私有格式取值，不是
        // 原始 GATT 封包，不檢查 byte[0]==0x51。
        if (len < 8) {
            return 0;
        }
        uint32_t measured_key = fora_protocol_decode_measured_key(value);
        out[0].type = VITAL_TYPE_SYSTOLIC;
        out[0].value = (float)value[4];
        out[0].device_measured_key = measured_key;
        out[0].source_kind = (uint8_t)kind;
        out[1].type = VITAL_TYPE_DIASTOLIC;
        out[1].value = (float)value[6];
        out[1].device_measured_key = measured_key;
        out[1].source_kind = (uint8_t)kind;
        out[2].type = VITAL_TYPE_PULSE_RATE;
        out[2].value = (float)value[7];
        out[2].device_measured_key = measured_key;
        out[2].source_kind = (uint8_t)kind;
        return 3;
    }

    if (len < 4 || value[0] != 0x51) {
        return 0; // 不是我們觸發指令的回應封包
    }

    if (kind == FORA_DEVICE_OXIMETER) {
        // 2026-08-03 實測回推（手指夾著、螢幕顯示 SpO2 97% / BPM 71-80 時
        // 收到 51 26 61 00 3c 4c a5 05）：byte[4]/byte[6]/byte[7] 目前不知道
        // 用途，先忽略。這個回應封包沒有像血壓計那樣附帶量測時間戳，
        // device_measured_key 只能填 0（見 common.h 該欄位的說明，storage.c
        // 會退回用數值+時間窗口的經驗法則判重）。
        if (len < 6) {
            return 0;
        }
        uint16_t spo2_raw = (((uint16_t)value[3] << 8) | value[2]) & 0x0FFF;
        out[0].type = VITAL_TYPE_SPO2;
        out[0].value = (float)spo2_raw;
        out[0].device_measured_key = 0;
        out[0].source_kind = (uint8_t)kind;
        out[1].type = VITAL_TYPE_PULSE_RATE;
        out[1].value = (float)value[5];
        out[1].device_measured_key = 0;
        out[1].source_kind = (uint8_t)kind;
        return 2;
    }

    uint16_t raw = (((uint16_t)value[3] << 8) | value[2]) & 0x0FFF;

    out[0].type = VITAL_TYPE_TEMPERATURE;
    out[0].value = (float)raw / 10.0f;
    out[0].device_measured_key = 0;
    out[0].source_kind = (uint8_t)kind;
    return 1;
}
