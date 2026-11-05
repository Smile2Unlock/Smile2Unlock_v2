# Windows camera and liveness diagnostics

The camera backend enumerates Media Foundation's native MJPEG/YUY2/NV12
media types, including rational frame rates. NV12 support targets MIPI/IPU
cameras such as the Meteor Lake Xiaomi webcam in #127; the reporter's actual
subtype and device behavior still require hardware diagnostics. It prefers
at least about 15 FPS and recognition-sized frames up to 1280×720. At equal
cadence/size group, the format order is MJPEG, YUY2, then NV12; MJPEG reduces
USB bandwidth. The next advertised mode is tried if a driver rejects one.

If no supported native candidate can be set, the source reader is recreated
with `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING = TRUE`. It requests YUY2 at
1280×720, 640×480, then 320×240, letting Media Foundation convert other source
formats. Neither this path nor incomplete-descriptor fallback requests an
arbitrary driver-default resolution. Video processing support still depends
on the installed driver and MF transforms.

NV12 is top-down: the Y and interleaved UV planes share a row stride, with UV
starting at `stride * height`. A contiguous locked buffer preserves
`MF_MT_DEFAULT_STRIDE` (or MF's computed default) and may have unused trailing
bytes; those bytes are not used to guess a larger stride. For `IMF2DBuffer`
surfaces, `ContiguousCopyTo` removes hardware pitch/height padding and the
published stride is the image width. Both planes are checked before libyuv
conversion. Only packed YUY2 uses the existing negative-stride row reversal;
NV12 planes are never reversed as packed rows.

`E_ACCESSDENIED`, sharing/busy errors, `MF_E_VIDEO_DEVICE_LOCKED` (the SDK's
locked-video-device constant), and camera preemption have a distinct
permission-or-busy outcome. Preview shows localized guidance to enable
**Settings > Privacy & security > Camera > Let desktop apps access your
camera**, or close other camera apps. Enrollment and desktop checks use the
same guidance. Other camera-open errors retain the generic unavailable result.

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
2. Export diagnostics, or inspect the structured `app.log` in
   `%LOCALAPPDATA%\Smile2Unlock\Logs` while the application is open (the
   system recognition agent logs under `%ProgramData%\Smile2Unlock\Logs`).
   Each open logs `camera.native_type` once per advertised native type with
   `index`, `subtype` fourcc, numeric `subtype_data1`, `width`, `height`, and
   `fps_numerator`/`fps_denominator`. `camera.native_types` ends the list with
   `count` and `supported`. No device names, symbolic links, image data, or
   account information are included in these events.
   Failed steps emit `camera.open_failed` with `step`, hexadecimal `hresult`,
   and `permission_or_busy`; rejected attempts may precede a successful open.
   `camera.open_path path=video_processing` identifies the conversion retry.
   A successful `camera.mode` records `format`, size, stride, cadence, and
   `path=native` or `path=video_processing`. The preview's
   `preview.camera_open_failed` includes the distinct `recognizer_error` when
   access is denied or the camera is occupied.
   Every five seconds, debug-level `preview.capture` records `fps`, while
   `preview.inference` records `ms`. Liveness logs `status`, `clarity`,
   `reality`, `score`, and `frames=.../10` periodically and on verdict changes.
3. A low capture `fps` with short inference time points to capture/driver/USB
   issues. Smooth capture with long inference `ms` points to model CPU cost.
   Repeated `clarity < 0.3` resets the window; persistent low `reality` prevents
   passing even when the frame rate is healthy.
4. Re-enroll, then test enrollment and sign-in separately. Repeat preview
   start/stop and check that the application's memory usage stabilizes.

If “4K” describes the monitor, record its scaling percentage as well; display
resolution and negotiated camera resolution are independent. A physical
camera/Windows run is required to confirm the achieved FPS and live-person
acceptance on the affected hardware.

## Regression checks

`su_capture_pipeline_test` covers NV12 ranking below MJPEG/YUY2 at equal
cadence, fast NV12 versus slow YUY2, even NV12 dimensions, 4K versus modest
capture modes, fractional frame rates, pending-frame replacement under slow
inference, duplicate-frame prevention, and waking a stopped consumer. On
Windows it also compiles the production MF translation unit without SeetaFace.
`su_pixel_convert_test` checks RGB/UV channel order, packed and padded NV12
stride, and rejection of truncated buffers, invalid stride and odd dimensions.
Run both focused tests on Linux, and cross-build the actual MF TU on MinGW:

```sh
xmake f -y -p linux -a x86_64 -m debug --with_slint=n --with_seetaface=n
xmake build su_capture_pipeline_test && xmake test su_capture_pipeline_test/default
xmake build su_pixel_convert_test && xmake test su_pixel_convert_test/default
xmake f -y -p mingw -a x86_64 -m release --with_slint=n --with_seetaface=n
xmake build su_capture_pipeline_test su_pixel_convert_test
xmake test su_capture_pipeline_test/default su_pixel_convert_test/default
```

The Windows test runner uses Wine when cross-testing on Linux. These checks
verify compilation and synthetic buffers, not camera hardware. The reporter
must still verify preview/enrollment/start-stop on the physical Xiaomi laptop,
then export diagnostics if opening still fails. Test denied desktop-camera
permission and exclusive use separately; both should show the localized advice.

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
