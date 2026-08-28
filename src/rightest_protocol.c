#include "rightest_protocol.h"

#include "fora_protocol.h" // FORA_DEVICE_RIGHTEST_GM700SB、fora_measurement_mode_t 共用（見該檔案的說明）

#include "btstack.h"

#include <string.h>

size_t rightest_protocol_build_command(uint8_t cmd, const uint8_t *data, uint8_t data_len, uint8_t *out) {
    out[0] = RIGHTEST_COMMAND_HEADER;
    out[1] = cmd;
    if (data_len > 0 && data != NULL) {
        memcpy(&out[2], data, data_len);
    }
    uint32_t sum = 0;
    for (uint8_t i = 0; i < (uint8_t)(data_len + 2); i++) {
        sum += out[i];
    }
    out[data_len + 2] = (uint8_t)(sum & 0xFF);
    return (size_t)data_len + 3;
}

bool rightest_protocol_verify_response(const uint8_t *frame, size_t frame_len) {
    if (frame_len < 2 || frame[0] != RIGHTEST_RETURN_HEADER) {
        return false;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < frame_len - 1; i++) {
        sum += frame[i];
    }
    return (uint8_t)(sum & 0xFF) == frame[frame_len - 1];
}

bool rightest_protocol_parse_record_summary(const uint8_t *frame, size_t frame_len, rightest_record_summary_t *out) {
    // TYPE 1 回應固定 11 bytes：0x4F 0x9E 0x00 0x00 DA_0..DA_5 CS（見
    // rightest_protocol.h 的說明，跟 TYPE 2 的 21 bytes 不一樣長）。
    if (frame_len != 11 || !rightest_protocol_verify_response(frame, frame_len)) {
        return false;
    }
    if (frame[1] != RIGHTEST_RETURN_READ_RECORD || frame[2] != 0x00 || frame[3] != 0x00) {
        return false;
    }
    out->total_count = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
    out->max_capacity = (uint16_t)frame[6] | ((uint16_t)frame[7] << 8);
    out->last_transmission_index = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
    return true;
}

bool rightest_protocol_parse_record(const uint8_t *frame, size_t frame_len, vital_record_t *out) {
    // TYPE 2 回應固定 21 bytes：0x4F 0x9E IND_L IND_H DA_0..DA_5 (10 bytes
    // reserved) CS，見協定文件 Read One Record 的表格。
    if (frame_len != 21 || !rightest_protocol_verify_response(frame, frame_len)) {
        return false;
    }
    if (frame[1] != RIGHTEST_RETURN_READ_RECORD) {
        return false;
    }
    uint16_t index = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
    if (index == 0) {
        return false; // TYPE 1 的回應不該被送進這裡解析，呼叫端邏輯有誤
    }

    uint8_t da0 = frame[4];
    uint8_t da1 = frame[5];
    uint8_t da2 = frame[6];
    uint8_t da3 = frame[7];
    uint8_t da4 = frame[8];
    uint8_t da5 = frame[9];

    // Hi 旗標（>600 mg/dL）：協定文件沒講這種情況 Glucose 欄位實際放什麼值，
    // 保守起見直接當無效讀值，不猜測、不上傳一個可能不準的數字，見
    // rightest_protocol.h 的說明。
    if ((da3 & 0x80) != 0) {
        return false;
    }
    // 品管測試（Control Solution）旗標，不是病人數值，比照 FORA QC 過濾邏輯。
    if ((da4 & 0x04) != 0) {
        return false;
    }

    // 日期/時間欄位佈局（協定文件 Read One Record Type2 Return Data
    // Description，公式跟文件的 TYPE 2 範例逐項核對過）：
    unsigned month = (unsigned)(((da1 & 0xC0) >> 4) + ((da0 & 0xC0) >> 6) + 1);
    unsigned day = (unsigned)((da0 & 0x1F) + 1);
    unsigned hour = (unsigned)(da1 & 0x1F);
    unsigned minute = (unsigned)(da2 & 0x3F);
    unsigned year = (unsigned)((da3 & 0x7F) + 2000);
    uint16_t glucose = (uint16_t)(((uint16_t)(da4 & 0x03) << 8) | da5);

    // 餐別標記（DA_4 bits5:3）：0=飯前、1=飯後，其餘（無餐/宵夜/睡前/運動/
    // 起床）簡化都當一般，見 rightest_protocol.h 的說明。
    uint8_t meal_bits = (uint8_t)((da4 >> 3) & 0x07);
    fora_measurement_mode_t mode;
    if (meal_bits == 0) {
        mode = FORA_MEASUREMENT_MODE_AC;
    } else if (meal_bits == 1) {
        mode = FORA_MEASUREMENT_MODE_PC;
    } else {
        mode = FORA_MEASUREMENT_MODE_GEN;
    }

    // 跟 fora_protocol_decode_measured_key() 完全相同的 bit-packing，見
    // rightest_protocol.h 的說明，讓既有的日期換算函式可以直接沿用。
    uint32_t year_offset = (uint32_t)(year - 2000);
    out->type = VITAL_TYPE_GLUCOSE;
    out->value = (float)glucose;
    out->device_measured_key = (year_offset << 20) | ((uint32_t)month << 16) |
                                ((uint32_t)day << 11) | ((uint32_t)hour << 6) | (uint32_t)minute;
    out->source_kind = (uint8_t)FORA_DEVICE_RIGHTEST_GM700SB;
    out->measurement_mode = (uint8_t)mode;
    return true;
}

bool rightest_protocol_parse_model_name(const uint8_t *frame, size_t frame_len, char out[RIGHTEST_MODEL_NAME_LEN + 1]) {
    size_t expected_len = 2 + RIGHTEST_MODEL_NAME_LEN + 1; // header + return-id + 5 字元 + checksum = 8 bytes
    if (frame_len != expected_len || !rightest_protocol_verify_response(frame, frame_len)) {
        return false;
    }
    if (frame[1] != RIGHTEST_RETURN_QUERY_MODEL_NAME) {
        return false;
    }
    memcpy(out, &frame[2], RIGHTEST_MODEL_NAME_LEN);
    out[RIGHTEST_MODEL_NAME_LEN] = '\0';
    return true;
}

bool rightest_protocol_matches_advertisement(const uint8_t *adv_data, uint8_t adv_len) {
    ad_context_t context;
    for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        uint8_t data_len = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);

        if (data_type != BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS &&
            data_type != BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS) {
            continue;
        }
        for (uint8_t i = 0; i + 2 <= data_len; i += 2) {
            uint16_t uuid16 = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8); // little-endian
            if (uuid16 == RIGHTEST_SERVICE_UUID16) {
                return true;
            }
        }
    }
    return false;
}

void rightest_reassembly_reset(rightest_reassembly_t *state) {
    state->length = 0;
    state->total_packets = 0;
    state->next_packet_index = 0;
    state->in_progress = false;
}

bool rightest_reassembly_feed(rightest_reassembly_t *state, const uint8_t *payload, uint16_t payload_len) {
    // 2026-08-28 實機測試發現：至少「meter-ID 自動推播」（16 bytes）完全
    // 沒有帶協定文件說的 2-byte 分包表頭，是原始內容直接送過來（不確定
    // 型號/總數/單筆記錄這幾個單包就裝得下的正式指令回應是否也一樣省略
    // 表頭）。用內容本身第一個 byte 是不是 Return Data Header（0x4F）判斷：
    // 是的話直接當成已經收完整的單包回應，不跑分包表頭邏輯；不是的話才
    // 照文件說的 [總封包數][目前第幾包] 表頭處理，兩種情況都能正確處理。
    if (payload_len > 0 && payload[0] == RIGHTEST_RETURN_HEADER) {
        if (payload_len > RIGHTEST_REASSEMBLY_MAX_BYTES) {
            rightest_reassembly_reset(state);
            return false;
        }
        rightest_reassembly_reset(state);
        memcpy(state->buffer, payload, payload_len);
        state->length = payload_len;
        return true;
    }

    if (payload_len < 2) {
        rightest_reassembly_reset(state);
        return false;
    }
    uint8_t total_packets = payload[0];
    uint8_t packet_index = payload[1];
    uint16_t chunk_len = (uint16_t)(payload_len - 2);

    if (!state->in_progress) {
        if (packet_index != 1) {
            return false; // 沒收到第一包，等下一輪從頭開始
        }
        rightest_reassembly_reset(state);
        state->in_progress = true;
        state->total_packets = total_packets;
        state->next_packet_index = 1;
    }

    if (total_packets != state->total_packets || packet_index != state->next_packet_index ||
        state->length + chunk_len > RIGHTEST_REASSEMBLY_MAX_BYTES) {
        // 表頭跟目前進度對不上、或超出緩衝區容量，整個放棄重來，避免半筆
        // 殘留資料跟下一輪答案混在一起。
        rightest_reassembly_reset(state);
        return false;
    }

    memcpy(&state->buffer[state->length], &payload[2], chunk_len);
    state->length += chunk_len;
    state->next_packet_index++;

    if (state->next_packet_index > state->total_packets) {
        state->in_progress = false; // 重組完成，呼叫端讀完 buffer 後要自己呼叫 reset() 準備下一筆
        return true;
    }
    return false;
}
