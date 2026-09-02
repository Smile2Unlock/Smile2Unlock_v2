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
    SuControlUsernameCap = 256,
    SuControlProfileIdCap = 64,
    SuControlProfileLabelCap = 128,
    SuControlSampleSourceCap = 49152,
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
    SuStatus_CryptoError = 9,
    SuStatus_KeyUnavailable = 10,
    SuStatus_MigrationRequired = 11,
} SuStatus;

typedef enum SuAccountKind {
    SuAccountKind_LinuxUid = 1,
    SuAccountKind_WindowsSid = 2,
} SuAccountKind;

typedef enum SuWindowsAccountKind {
    SuWindowsAccountKind_Local = 1,
    SuWindowsAccountKind_Microsoft = 2,
} SuWindowsAccountKind;

typedef struct SuEncryptedStoreContext {
    const uint8_t* master_key;
    uintptr_t master_key_len;
    uint32_t key_version;
    uint32_t account_kind;
    uint32_t linux_uid;
    const char* windows_sid;
} SuEncryptedStoreContext;

typedef enum SuControlMessageType {
    SuControlMessageType_Authenticate = 1,
    SuControlMessageType_Status = 2,
    SuControlMessageType_Cancel = 3,
    SuControlMessageType_StorageStatus = 4,
    SuControlMessageType_ListProfiles = 5,
    SuControlMessageType_EnrollProfile = 6,
    SuControlMessageType_DeleteProfile = 7,
    SuControlMessageType_MigrateProfiles = 8,
    SuControlMessageType_VerifyProfile = 9,
} SuControlMessageType;

typedef struct SuControlRequest {
    SuControlMessageType msg_type;
    uint64_t request_id;
    uint64_t target_request_id;
    uint8_t username[SuControlUsernameCap];
    uint8_t profile_id[SuControlProfileIdCap];
    uint8_t label[SuControlProfileLabelCap];
    uint8_t face_sample_source[SuControlSampleSourceCap];
    bool liveness_ok;
} SuControlRequest;

typedef struct SuAuthDecision {
    SuStatus status;
    bool accepted;
} SuAuthDecision;

typedef struct SuCoreConfig {
    uint32_t version;
    int32_t selected_camera;
    float recognition_threshold;
    bool liveness_detection;
    float liveness_threshold;
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
    bool liveness_ok;
    uint32_t profile_count;
    uint8_t best_profile_id[SuFaceProfileIdCap];
    uint8_t best_profile_label[SuFaceProfileLabelCap];
    uint8_t reason[SuFaceAuthReasonCap];
} SuFaceAuthReport;

uint32_t su_core_version_major(void);
SuStatus su_core_parse_control_request(
    const uint8_t* input,
    uintptr_t input_len,
    SuControlRequest* out_request);
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
SuFaceAuthDecision su_core_authenticate_face_sample_with_liveness(
    const char* store_path,
    const char* face_sample_source,
    float threshold,
    bool liveness_ok);
SuStatus su_core_authenticate_face_sample_report_json(
    const char* store_path,
    const char* face_sample_source,
    float threshold,
    uint8_t* out_buffer,
    uintptr_t buffer_len,
    uintptr_t* out_required_len);
SuStatus su_core_authenticate_face_sample_report_json_with_liveness(
    const char* store_path,
    const char* face_sample_source,
    float threshold,
    bool liveness_ok,
    uint8_t* out_buffer,
    uintptr_t buffer_len,
    uintptr_t* out_required_len);
SuFaceAuthReport su_core_authenticate_face_sample_report(
    const char* store_path,
    const char* face_sample_source,
    float threshold);
SuFaceAuthReport su_core_authenticate_face_sample_report_with_liveness(
    const char* store_path,
    const char* face_sample_source,
    float threshold,
    bool liveness_ok);
SuStatus su_core_encrypted_enroll_face_profile(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    const char* label,
    const char* face_sample_source);
SuStatus su_core_encrypted_delete_face_profile(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    const char* profile_id,
    bool* out_deleted);
SuStatus su_core_encrypted_list_face_profiles_json(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    uint8_t* out_buffer,
    uintptr_t buffer_len,
    uintptr_t* out_required_len);
SuStatus su_core_encrypted_list_face_profile_summaries(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    SuFaceProfileSummary* out_profiles,
    uintptr_t profile_capacity,
    uintptr_t* out_profile_count);
SuFaceAuthReport su_core_encrypted_authenticate_face_sample_report(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    const char* face_sample_source,
    float threshold,
    bool liveness_ok);
SuStatus su_core_migrate_plaintext_face_profiles(
    const SuEncryptedStoreContext* context,
    const char* legacy_path,
    const char* encrypted_path,
    bool* out_migrated);
SuStatus su_core_store_windows_logon_secret(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    uint32_t account_kind,
    const char* canonical_username,
    const uint16_t* password,
    uintptr_t password_len,
    uint64_t* out_generation);
SuStatus su_core_prepare_windows_logon_secret(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    uint64_t request_id,
    uint32_t logon_session_id,
    uint16_t* out_password,
    uintptr_t password_capacity,
    uintptr_t* out_password_len);
SuStatus su_core_mark_windows_logon_secret_stale(
    const SuEncryptedStoreContext* context,
    const char* store_path);
SuStatus su_core_clear_windows_logon_secret(
    const SuEncryptedStoreContext* context,
    const char* store_path,
    bool* out_cleared);

#ifdef __cplusplus
}
#endif
