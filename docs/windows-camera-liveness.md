# Windows camera and liveness diagnostics

The camera backend enumerates Media Foundation's native MJPEG/YUY2 media
types, including rational frame rates. It prefers at least about 15 FPS and
recognition-sized frames up to 1280×720, with MJPEG preferred at comparable
cadence to reduce USB bandwidth. It tries the next advertised mode if a driver
rejects the first. Cameras with incomplete descriptors get an explicit small
resolution fallback; the arbitrary default resolution is not requested.

Preview capture runs independently of anti-spoofing. Inference consumes the
latest immutable frame once, and both the inference mailbox and pending UI
update retain at most one frame. The displayed face box expires after 500 ms
and the displayed score after two seconds. Authentication evaluates its own
captured images and does not use preview overlays.

Public image buffers remain RGB/RGBA. At the SeetaFace boundary they are
converted to the SDK's required BGR format. Existing profiles were extracted
using reversed red/blue channels and may no longer match consistently;
re-enroll profiles after upgrading. Liveness still requires ten clear frames
and the existing 0.8 average reality gate. No detection threshold was lowered.

## Reproduce on Windows 11

1. Launch the updated application, enable liveness, select the color camera,
   and run preview for at least 20 seconds with one person in good lighting.
2. Inspect `C:\Windows\Temp\su_stderr.log` while the application is open.
   Camera opening logs `[camera] MF mode=... size=... fps=numerator/denominator`.
   Every five seconds preview logs `capture_fps` and `inference_ms`. Liveness
   logs `status`, `clarity`, `reality`, `score`, and `frames=.../10` periodically
   and on verdict changes. The log is overwritten on each GUI launch.
3. A low `capture_fps` with short inference time points to capture/driver/USB
   issues. Smooth capture with long `inference_ms` points to model CPU cost.
   Repeated `clarity < 0.3` resets the window; persistent low `reality` prevents
   passing even when the frame rate is healthy.
4. Re-enroll, then test enrollment and sign-in separately. Repeat preview
   start/stop and check that the application's memory usage stabilizes.

If “4K” describes the monitor, record its scaling percentage as well; display
resolution and negotiated camera resolution are independent. A physical
camera/Windows run is required to confirm the achieved FPS and live-person
acceptance on the affected hardware.

## Regression checks

`su_capture_pipeline_test` covers MJPEG versus slow YUY2, 4K versus modest
capture modes, fractional frame rates, pending-frame replacement under slow
inference, duplicate-frame prevention, and waking a stopped consumer. On
Windows it also compiles the production MF translation unit without SeetaFace.

`su_seetaface_pipeline_smoke_test` compares the backend's RGB input with direct
SDK BGR landmark/feature/anti-spoofing calls. Its SDK agreement check detects
channel reversal that ordinary self-comparison tests cannot detect. Still
image fixtures verify SDK agreement, not physical live-person acceptance.

Nokhwa uses Media Foundation on Windows too. Replacing the backend adds an
FFI boundary and dependency without directly fixing media-type selection,
sample ownership, or the SDK channel ordering. Keep the existing backend for
this fix; reassess replacement if device testing identifies an additional
backend compatibility problem.

References: [SeetaFace input layout](https://github.com/seetafaceengine/SeetaFaceTutorial/blob/master/README.md#211-结构定义),
[Microsoft capture format selection](https://learn.microsoft.com/en-us/windows/win32/medfound/how-to-set-the-video-capture-format),
[Nokhwa Windows backend](https://github.com/l1npengtul/nokhwa/blob/senpai/nokhwa-bindings-windows/src/lib.rs).
