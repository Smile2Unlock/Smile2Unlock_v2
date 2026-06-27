#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SuFaceProfileIdCap = 64,
    SuFaceProfileLabelCap = 128,
    SuFaceAuthReasonCap = 128,
};

typedef enum SuStatus {
    SuStatus_Ok = 0,
    SuStatus_NullArgument = 1,
    SuStatus_InvalidUtf8 = 2,
    SuStatus_UserDenied = 3,
    SuStatus_IoError = 4,
    SuStatus_ParseError = 5,
    SuStatus_WriteError = 6,
    SuStatus_InvalidArgument = 7,
    SuStatus_BufferTooSmall = 8,
} SuStatus;

typedef struct SuAuthDecision {
    SuStatus status;
    bool accepted;
} SuAuthDecision;

typedef struct SuCoreConfig {
    uint32_t version;
    int32_t selected_camera;
    float recognition_threshold;
    bool liveness_detection;
    uint32_t preview_fps;
} SuCoreConfig;

typedef struct SuFaceAuthDecision {
    SuStatus status;
    bool accepted;
    float score;
    uint32_t profile_count;
} SuFaceAuthDecision;

typedef struct SuFaceProfileSummary {
    uint8_t id[SuFaceProfileIdCap];
    uint8_t label[SuFaceProfileLabelCap];
    uint64_t created_at_unix;
} SuFaceProfileSummary;

typedef struct SuFaceAuthReport {
    SuStatus status;
    bool accepted;
    float score;
    float threshold;
    uint32_t profile_count;
    uint8_t best_profile_id[SuFaceProfileIdCap];
    uint8_t best_profile_label[SuFaceProfileLabelCap];
    uint8_t reason[SuFaceAuthReasonCap];
} SuFaceAuthReport;

uint32_t su_core_version_major(void);
SuAuthDecision su_core_evaluate_auth(
    const char* username,
    float similarity,
    float threshold,
    bool liveness_ok);
SuStatus su_core_default_threshold(float* out_threshold);
SuCoreConfig su_core_default_config(void);
SuStatus su_core_load_config(const char* path, SuCoreConfig* out_config);
SuStatus su_core_save_config(const char* path, const SuCoreConfig* config);
SuStatus su_core_enroll_face_profile(
    const char* store_path,
    const char* label,
    const char* face_sample_source);
SuStatus su_core_delete_face_profile(
    const char* store_path,
    const char* profile_id,
    bool* out_deleted);
SuStatus su_core_list_face_profiles_json(
    const char* store_path,
    uint8_t* out_buffer,
    uintptr_t buffer_len,
    uintptr_t* out_required_len);
SuStatus su_core_list_face_profile_summaries(
    const char* store_path,
    SuFaceProfileSummary* out_profiles,
    uintptr_t profile_capacity,
    uintptr_t* out_profile_count);
SuFaceAuthDecision su_core_authenticate_face_sample(
    const char* store_path,
    const char* face_sample_source,
    float threshold);
SuStatus su_core_authenticate_face_sample_report_json(
    const char* store_path,
    const char* face_sample_source,
    float threshold,
    uint8_t* out_buffer,
    uintptr_t buffer_len,
    uintptr_t* out_required_len);
SuFaceAuthReport su_core_authenticate_face_sample_report(
    const char* store_path,
    const char* face_sample_source,
    float threshold);

#ifdef __cplusplus
}
#endif
