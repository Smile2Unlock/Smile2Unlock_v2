#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SuStatus {
    SuStatus_Ok = 0,
    SuStatus_NullArgument = 1,
    SuStatus_InvalidUtf8 = 2,
    SuStatus_UserDenied = 3,
    SuStatus_IoError = 4,
    SuStatus_ParseError = 5,
    SuStatus_WriteError = 6,
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

#ifdef __cplusplus
}
#endif

