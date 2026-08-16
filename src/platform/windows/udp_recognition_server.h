// Plain (non-module) TU: winsock2.h is safe to include here.
//
// UDP face-recognition server for the Windows Credential Provider.
// Protocol parity with the C++ baseline and the Rust provider
// (credential_provider_rs/src/recognition.rs):
//   request: UDP 127.0.0.1:51236, UdpAuthRequestPacket (magic "AUTH")
//   status:  UDP 127.0.0.1:51234, UdpStatusPacket (magic 0x8581DAF3)
//
// The server thread only parses packets and sends replies; the actual
// recognition work runs in the callback provided by the caller (which owns
// the camera and the profile store).

#ifndef SU_PLATFORM_WINDOWS_UDP_RECOGNITION_SERVER_H_
#define SU_PLATFORM_WINDOWS_UDP_RECOGNITION_SERVER_H_

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// RecognitionStatus (common/models/recognition_status.h).
enum {
    SU_RS_IDLE = 0,
    SU_RS_RECOGNIZING = 1,
    SU_RS_SUCCESS = 2,
    SU_RS_FAILED = 3,
    SU_RS_TIMEOUT = 4,
    SU_RS_RECOGNITION_ERROR = 5,
    SU_RS_FACE_DETECTED = 6,
    SU_RS_PROCESS_ENDED = 7,
};

// Wire constants (C++ parity with the Rust provider).
enum {
    SU_UDP_AUTH_REQUEST_MAGIC = 0x41555448,  // "AUTH"
    SU_UDP_AUTH_REQUEST_VERSION = 1,
    SU_UDP_STATUS_MAGIC = 0x8581DAF3,
    SU_UDP_STATUS_VERSION = 2,
    SU_UDP_CP_STATUS_PORT = 51234,
    SU_UDP_AUTH_REQUEST_PORT = 51236,
    SU_UDP_REQ_START_RECOGNITION = 1,
    SU_UDP_STATUS_USERNAME_CAP = 64,
};

// Performs one recognition attempt for the request. Must return an SU_RS_*
// code; on SU_RS_SUCCESS it should fill out_username (NUL-terminated,
// capacity SU_UDP_STATUS_USERNAME_CAP). Runs on the server thread.
typedef int (*SuUdpRecognizeCallback)(
    uint32_t session_id,
    const char* username_hint,
    char* out_username,
    void* userdata);

// Starts the server thread (single instance; subsequent calls are no-ops).
// Returns 0 on success.
int su_udp_start_recognition_server(
    SuUdpRecognizeCallback callback,
    void* userdata);

// Requests the server thread to stop and joins it.
void su_udp_stop_recognition_server(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SU_PLATFORM_WINDOWS_UDP_RECOGNITION_SERVER_H_
