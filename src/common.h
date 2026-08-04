#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_SSID_MAX_LEN    32
#define WIFI_PASS_MAX_LEN    64
#define PATIENT_NAME_MAX_LEN 64
#define PATIENT_ID_MAX_LEN   32
#define CASE_MANAGER_MAX_LEN 64

// 熱點設定模式收集的裝置設定。
typedef struct {
    char wifi_ssid[WIFI_SSID_MAX_LEN];
    char wifi_password[WIFI_PASS_MAX_LEN];
    char patient_name[PATIENT_NAME_MAX_LEN];
    char patient_id[PATIENT_ID_MAX_LEN];
    char case_manager_info[CASE_MANAGER_MAX_LEN];
    bool valid;
} device_config_t;

typedef enum {
    VITAL_TYPE_UNKNOWN = 0,
    VITAL_TYPE_TEMPERATURE,
    VITAL_TYPE_SPO2,
    VITAL_TYPE_PULSE_RATE,
    VITAL_TYPE_SYSTOLIC,
    VITAL_TYPE_DIASTOLIC,
} vital_type_t;

typedef enum {
    UPLOAD_STATUS_PENDING = 0,
    UPLOAD_STATUS_UPLOADED,
    UPLOAD_STATUS_FAILED,
} upload_status_t;

// 一筆生理量測紀錄。
typedef struct {
    uint64_t received_at_ms;
    uint64_t uploaded_at_ms;
    vital_type_t type;
    float value;
    upload_status_t status;
} vital_record_t;

#endif // COMMON_H
