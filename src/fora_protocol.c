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
        bool is_fora = false;
        for (uint8_t i = 0; i + 4 <= data_len; i++) {
            if (memcmp(&data[i], "FORA", 4) == 0) {
                is_fora = true;
                break;
            }
        }
        if (!is_fora) {
            continue;
        }

        // 血氧計實測名稱是 "FORA O2"、血壓計是 "FORA D40"。額溫槍的實際廣播
        // 名稱從沒有拿實機驗證過，"IR42"（裝置型號本身）是目前最合理的猜測——
        // 如果之後發現額溫槍連不上，要用實機掃描比對出真正的字串再回來改這裡。
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
        bool is_thermometer = false;
        for (uint8_t j = 0; j + 4 <= data_len; j++) {
            if (memcmp(&data[j], "IR42", 4) == 0) {
                is_thermometer = true;
                break;
            }
        }
        // MD6 六合一測試儀，2026-08-26 實機確認廣播名稱含 "MD6"，見
        // fora_protocol.h FORA_DEVICE_MD6 的說明。
        bool is_md6 = false;
        for (uint8_t j = 0; j + 3 <= data_len; j++) {
            if (data[j] == 'M' && data[j + 1] == 'D' && data[j + 2] == '6') {
                is_md6 = true;
                break;
            }
        }

        // 每一種認得的型號都要明確比對到關鍵字才算數，不接受「名稱含 FORA、
        // 但比對不出型號」就預設當成額溫槍——這是舊版的行為，會讓還沒支援
        // 的裝置被誤判成額溫槍、誤用額溫槍的協定解析回應（已經實際發生過
        // 一次：MD6 的回應被誤解成一筆體溫讀值上傳出去，這也是當初加上
        // FORA_DEVICE_MD6 明確分類、不再讓它落入這個 fallback 的原因）。
        if (!is_oximeter && !is_blood_pressure && !is_thermometer && !is_md6) {
            return false;
        }
        if (out_kind != NULL) {
            *out_kind = is_oximeter ? FORA_DEVICE_OXIMETER
                      : is_blood_pressure ? FORA_DEVICE_BLOOD_PRESSURE
                      : is_md6 ? FORA_DEVICE_MD6
                      : FORA_DEVICE_THERMOMETER;
        }
        return true;
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

        // 這台裝置是血壓血糖二合一，「問目前這一筆記錄」的回應可能是血壓、
        // 也可能是血糖，靠 byte[2] 的 bit7 分辨（0=血糖、1=血壓），見
        // fora_protocol.h 開頭的協定說明。
        if ((value[2] & 0x80) == 0) {
            // 血糖格式：byte[4..5] 是 16-bit 小端血糖值，byte[6] 是環境溫度
            // （目前不用），byte[7] 低 6 bit 是 codeNo（不用）、高 2 bit 是
            // 量測情境（一般/飯前/飯後/QC，跟 MD6 共用同一套 BloodGlucose
            // class，見 fora_protocol.h 的 fora_measurement_mode_t 說明）。
            int glucose = (int)value[5] * 256 + (int)value[4];
            if (glucose == 65535 || glucose == 255) {
                // 官方程式對這兩個特殊值的判斷：不是真正的血糖數字（感測器
                // 誤差/品管測試等情況），視為無效讀值，不當成一筆資料回傳。
                return 0;
            }
            uint8_t context_code = (value[7] >> 6) & 0x03;
            if (context_code == 3) {
                return 0; // QC（品管/對照液測試），不是病人數值，不回傳。
            }
            out[0].type = VITAL_TYPE_GLUCOSE;
            out[0].value = (float)glucose;
            out[0].device_measured_key = measured_key;
            out[0].source_kind = (uint8_t)kind;
            out[0].measurement_mode = context_code;
            return 1;
        }

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

    if (kind == FORA_DEVICE_MD6) {
        // 呼叫端已經把兩次指令的回應接成 8 bytes 了，跟血壓計共用同一套
        // 8-byte 記錄格式（PROJECT_PLAN.md 第 6.5 節），日期時間解碼方式也
        // 完全一樣，差別在 byte[7] 的用途換成「量測情境＋項目代碼」。
        if (len < 8) {
            return 0;
        }
        uint32_t measured_key = fora_protocol_decode_measured_key(value);

        uint16_t raw_value = (uint16_t)value[4] | ((uint16_t)value[5] << 8);
        if (raw_value == 65535 || raw_value == 255) {
            // 跟血糖一樣的無效讀值判斷（見上面血壓計分支血糖格式的說明）。
            return 0;
        }

        // bits 2-5 = 這筆記錄是哪一項。MD6 是「六合一」機型，只會出現血糖/
        // HCT/酮體/尿酸/總膽固醇/血紅素這 6 種代碼；12(乳酸)/13(三酸甘油脂)
        // 是同一套協定 class 給其他型號用的，MD6 不會回報，遇到就跳過。
        uint8_t item_code = (value[7] >> 2) & 0x0F;
        vital_type_t type;
        switch (item_code) {
            case 0:  type = VITAL_TYPE_GLUCOSE; break;
            case 6:  type = VITAL_TYPE_HCT;     break;
            case 7:  type = VITAL_TYPE_KETONE;  break;
            case 8:  type = VITAL_TYPE_UA;      break;
            case 9:  type = VITAL_TYPE_CHOL;    break;
            case 11: type = VITAL_TYPE_HB;      break;
            default: return 0; // 認不出的項目代碼，不回傳任何記錄。
        }

        // bits 6-7 = 量測情境：0=一般、1=飯前(AC)、2=飯後(PC)、3=QC（品管/
        // 對照液測試），見 fora_protocol.h 的 fora_measurement_mode_t 說明。
        // 只在血糖套用 QC 過濾。**這裡的判斷來回改過兩次，這次是最終確認**：
        // (1) 一開始只在血糖過濾，因為往回翻頁抓到一筆 HCT=50（context
        //     bits==3）數字看起來合理，臆測不是 QC；
        // (2) 反編譯官方 PC 端程式（BloodGlucose class）發現官方碼對全部
        //     項目統一套用這個過濾，改成統一套用——但反編譯出來的只是「PC
        //     軟體怎麼分類/標示」，不代表官方軟體看到 QC 分類就會把記錄
        //     丟棄不存，這個推論當時沒有根據；
        // (3) 2026-08-26 使用者直接核對裝置螢幕：那 2 筆 HCT（46、50）**螢幕
        //     上完全沒有顯示 QC 標記**，就是正常讀值——證實 context bits 對
        //     HCT（可能其餘 4 項也是）不是「有沒有做 QC」的意思，改回只在
        //     血糖套用，這次有實機畫面證據，不是臆測。
        uint8_t context_code = (value[7] >> 6) & 0x03;
        if (type == VITAL_TYPE_GLUCOSE && context_code == 3) {
            return 0;
        }

        out[0].type = type;
        out[0].value = (float)raw_value;
        out[0].device_measured_key = measured_key;
        out[0].source_kind = (uint8_t)kind;
        out[0].measurement_mode = context_code;

        if (type == VITAL_TYPE_HCT) {
            // Hb（血紅素）這台裝置/這種試片沒有獨立的 BLE 記錄可以要——往回
            // 翻頁抓過裝置回報的全部記錄，只出現過血糖跟 HCT 兩種 item
            // code，從來沒有 item_code==11——但裝置螢幕每次都會跟 HCT 一起
            // 顯示一個 Hb 數字。2026-08-26 核對 2 個實機樣本：HCT=46→螢幕
            // 顯示 Hb=15.6、HCT=50→螢幕顯示 Hb=17，兩個樣本都精確吻合
            // Hb=HCT×0.34 這個血液學常用換算係數（見 PROJECT_PLAN.md 第
            // 6.5 節）。這裡用同樣公式算出一筆 VITAL_TYPE_HB 讓需要這個
            // 欄位的下游拿得到值——**這是韌體自己算出來的估計值，不是裝置
            // 量到的**，只有 2 個樣本驗證過。如果裝置以後真的會回報
            // item_code==11 的真實 Hb 記錄，storage.c 的覆蓋邏輯會用同一個
            // measured_key 蓋掉這裡算出來的估計值，換成真的測量值。
            out[1].type = VITAL_TYPE_HB;
            out[1].value = out[0].value * 0.34f;
            out[1].device_measured_key = measured_key;
            out[1].source_kind = (uint8_t)kind;
            out[1].measurement_mode = context_code;
            return 2;
        }
        return 1;
    }

    if (len < 4 || value[0] != 0x51) {
        return 0; // 不是我們觸發指令的回應封包
    }

    if (kind == FORA_DEVICE_OXIMETER) {
        // byte[4]/byte[6]/byte[7] 用途未知，忽略。這個回應封包沒有像血壓計
        // 那樣附帶量測時間戳，device_measured_key 只能填 0（見 common.h 該
        // 欄位的說明，storage.c 會退回用數值+時間窗口的經驗法則判重）。
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
