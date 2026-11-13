# Backlight-aware exposure (issue #137)

## Problem

With a bright window behind the user, the camera's auto exposure meters the
whole frame and the face comes out nearly black (measured face luma about
12/255 against about 193 for the window), so face recognition fails.

## Approach

A small closed loop meters the face instead of the frame and nudges camera
controls (one at a time, escalating along a chain) until the face sits in a
usable band. The window is allowed to blow
out; only the face matters.

- Region: the inner 70% of the face box once a face is found, otherwise a
  centred window (40% x 50% of the frame) where a seated user's face normally is.
- Target: mean face luma within `[90, 150]` (`ExposureTuning`). Too dark steps
  the active knob up; too bright, or more than 5% of face samples clipped (`>= 250`), steps
  down; inside the band the loop holds and touches nothing.
- Settle: after every change the loop skips the next `settle_observations`
  (4) observations and also waits at least `min_settle_time` (500 ms) of
  monotonic time, so the frame rate does not change how long the camera gets
  to react. `next_exposure_step` stays pure: the caller passes the time in.
- Anti flip-flop: the loop remembers the kind, direction and previous level of
  its last change. A decision that reverses direction and would land on the
  level it just left holds instead. That accepts a slightly out-of-band face
  rather than hunting between two levels (the Windows 2x exposure
  quantisation, or a clip-driven lower/raise cycle). The memory lapses after
  `flip_flop_memory` (30 s), so a scene that really changed is corrected
  later instead of being held forever.
- Normal-light users usually see no change: with the face in band the loop
  holds, and the camera meters the face or centre region. It is not a strict
  no-op: a pale or very bright face (mean above 150, or more than 5% clipped)
  in a bright room is "too bright" too, and can lower `ev_bias` (or take
  backlight compensation down) even though nothing is wrong with the lighting.

The decision logic is the pure, constexpr function `next_exposure_step` in
`src/recognizer/exposure/exposure_control.h`, unit tested by
`su_exposure_control_test`.

## Mechanism priority and the escalation chain

1. Metering region of interest (the camera's own auto exposure keeps running).
2. An ordered chain of level knobs. The camera reports every knob it supports
   (`ExposureCapabilities::knobs`), in this preference order:
   1. EV bias (`V4L2_CID_AUTO_EXPOSURE_BIAS`; skipped if disabled or inactive)
   2. Backlight compensation (`V4L2_CID_BACKLIGHT_COMPENSATION` on Linux,
      `VideoProcAmp_BacklightCompensation` on Windows)
   3. Manual exposure (`V4L2_CID_EXPOSURE_AUTO` -> manual plus
      `V4L2_CID_EXPOSURE_ABSOLUTE`; `CameraControl_Exposure` on Windows). Last
      resort: auto exposure is switched off only at the first level change,
      never at open, so the loop must keep tracking the scene. The first write
      engages manual mode at the level auto exposure had already chosen,
      unscaled (the face luma says nothing about the ratio until manual is in
      effect). After that exposure time moves proportionally toward the middle
      of the band (ratio clamped to `[0.5, 2]`, at least one step) and is
      capped at 1/30 s (333 in 100 microsecond units) and at the device
      maximum. Too bright while auto still rules: nothing is done.

A single knob saturates: backlight compensation is often only 0..1 or 0..2
(the UVC webcam used for the hardware check: 0..2) and a face at luma ~12/255
needs more. So `next_exposure_step` escalates:

- Brighten: step the active knob. When it is at its maximum, escalate to the
  next knob in the chain, starting from its initial level, and step that one.
- Darken (too bright, or clipped): step the highest escalated knob down. Once
  an escalated knob is back at its initial level the loop de-escalates: it emits
  a `restore` for that knob (manual exposure hands control back to the camera's
  auto exposure at its original mode and value) and the previous knob becomes
  active. This also holds for a manual-exposure knob that is the first or only
  knob of the chain: returning to its initial level re-enables auto. Other
  first knobs may go below their initial level.
- The state keeps one level per knob kind plus the active (highest escalated)
  kind; settle/hysteresis/clipping rules are unchanged, and nothing is touched
  while the face is in band. An empty chain never acts.
- Each step is one `ExposureChange { kind, set | restore, level }`; the camera
  API is `set_exposure_level(kind, level)` and `reset_exposure_knob(kind)`.
- Each camera write returns why it failed (see "Error policy"). The loop state
  keeps the level the camera APPLIED (drivers snap requests to their own
  grid), not the level requested.

## Error policy

`ExposureWriteError { unsupported, transient, rejected }`. On V4L2 `EINTR` is
retried in a loop (bounded), `EBUSY`, `EAGAIN` and `EIO` are transient and
anything else is rejected; the camera layer logs
`camera.exposure_write_failed` with the control id, value and errno.

The service drops a knob from the session chain (the loop then skips or
escalates past it) only after a rejected error or after 3 consecutive transient
errors; a transient failure keeps the loop state so the same change is retried
at the next observation. Dropping makes a best-effort reset of the knob and
logs `camera.exposure_knob_failed` with the error and the reset result (the
camera layer logs the errno of a failed restore as
`camera.exposure_restore_failed`). A metering region that is rejected disables
the region for the session.

## Restore guarantee

UVC cameras keep control values in firmware, so changes would leak into other
apps. Every control touched is read at `open()` and restored in reverse chain order
by `release()` and the destructor (and a knob the loop de-escalates out of is
restored immediately): absolute exposure before auto exposure is
re-enabled. For the ROI, the rect goes back before the auto bit when the
original auto bit was set (so re-enabling never meters a stale rect), and
after it when it was clear. A control stays marked as changed until its
restore succeeds: a failed reset (or a knob dropped by the service) is retried
when the camera closes, and every control is attempted even if an earlier one
failed. A failed restore is logged as `camera.exposure_restore_failed` with
the errno. Probe failures never fail `open()`. The EV-bias integer menu is
only used when every index between its minimum and maximum is a real menu
entry (`VIDIOC_QUERYMENU`); a sparse menu skips the knob.

### Paths that cannot restore

Restoring needs the process to run its destructors.

- Linux `su_authd` and the Slint GUI install SIGTERM/SIGINT handlers
  (`common/process/shutdown_signal.h`; the handler only sets a flag and writes
  a pipe byte, and the signal mask is untouched so child processes keep default
  signal behaviour). `su_authd` polls that pipe together with its listening
  socket, stops accepting, tells in-flight authentications to stop at their
  next frame, waits (up to 10 s) for them so each camera guard restores the
  controls, and exits with status 0 (systemd treats that as a clean stop). The
  GUI quits the Slint event loop from a watcher thread, so `main` returns and
  the preview controller closes the camera.
- SIGKILL, crashes (including systemd escalating to SIGKILL after the stop
  timeout) and power loss cannot restore: the camera keeps the last value the
  loop wrote until something resets it or it is replugged. On Windows,
  `TerminateProcess` likewise cannot restore.

Hardware check (UVC "USB2.0 HD UVC WebCam", backlight_compensation 0..2 +
manual exposure + ROI): `v4l2-ctl --list-ctrls` is identical before and after a
session; while open, the ROI rect, ROI auto bit and backlight_compensation were
observed changed. The escalation path (backlight 0 -> 2, then manual exposure)
and the de-escalation back to auto exposure were exercised through the camera
API with synthetic dark/bright luma.

## Low-light detect retry

Separate from the loop, `seetaface_backend.cpp` retries face detection on a
brightened copy so a backlit face is found at all (the loop needs a face box to
meter). Only when the first pass finds no face and the centre mean luma is
below 70, a gamma LUT is applied to a copy used by the detector alone;
landmarks, liveness and the feature extractor always run on the original
pixels so templates and scores do not shift. The detector input is BGR, so the
centre luma is measured with `PixelOrder::bgr` (`measure_luma` defaults to RGB;
reading BGR data as RGB would swap the red and blue weights).

## User switch

Settings > Recognition policy has a "Backlight compensation" switch (config key
`backlight_compensation`, default on). It covers both halves of the feature:
the exposure loop and the low-light detection retry.

OFF means the program behaves as it did before the feature:

- No camera control is ever written except restores (see below).
- Whatever the current session already changed is put back immediately
  (`RecognizerService::set_backlight_compensation(false)` resets every knob
  and the metering region under `camera_mutex_`; the setter may be called from
  the UI thread while preview or authentication threads run).
- A restore that fails is logged (`camera.exposure_restore_failed`, with the
  error class) and, when the error is `transient`, recorded as restore-pending
  (`RestorePending`: the knobs and the region still to put back). While the
  switch is off, later `observe_exposure` calls (the camera keeps delivering
  frames) retry only those restores, under `camera_mutex_` and throttled by the
  loop's `min_settle_time`. Retrying stops once everything succeeded; a
  `rejected` or `unsupported` error is logged once and not retried. No other
  control is ever written while off. `close()` still restores everything the
  camera layer marked as touched.
- Putting a control back is a brightness step, so the service resets the
  liveness window itself: `set_backlight_compensation` does so when the restore
  actually changed something, and `observe_exposure` returns true when a
  pending-restore retry succeeded (callers already reset liveness on true).
  Callers need no knowledge of the toggle.
- The low-light retry is skipped; the first detector pass is final, so a frame
  only the retry could have found yields no face and no face box.
- Turning it ON again restarts the loop from its initial state, with the same
  session-generation rules (the generation is not bumped; only the loop state,
  applied region and failure counters are cleared).

Defaults and compatibility:

- Config (`config.toml`): the key is `#[serde(default)]` true, so files written
  before the switch keep it on. `SuCoreConfig` gained `backlight_compensation`
  as its last field (struct size 52 to 56, asserted on the C and Rust sides).
- Windows registry: `BacklightCompensationEnabled` (REG_DWORD, 0 or 1) next to
  `LivenessEnabled`, under `HKLM\SOFTWARE\Smile2Unlock\Recognition\<SID>`. An
  absent or out-of-range value means on, so existing installs stay on.
- Windows service protocol: `RecognitionSettingsPayload` grew from 44 to 48
  bytes (new last field, default 1). The service also accepts the 44-byte
  legacy payload and then leaves the stored value alone. A new client talking
  to an old service is rejected by the old exact-size check, so the GUI and
  service must be upgraded together.
- Recognition agent request: layout, size (52) and version (2) are unchanged.
  The switch is the flag bit `kBacklightCompensationDisabled`, set only when
  the user turned it off. On is the all-zero flags word, so an agent that
  predates the switch still accepts every request except the "off" one, which
  it rejects as an unknown flag.
- The Windows service and the recognition agent must ship together (they
  always do, in one installer). An old agent rejects the new
  `kBacklightCompensationDisabled = 2` flag with `kInvalidRequest`, which
  only affects the OFF case. The protocol version was deliberately not bumped:
  a bump would break every mixed install, not only the OFF case.
- Image-file enrollment applies the saved switch to the recognizer before
  feature extraction (also applied once at GUI startup), so a saved OFF
  survives a restart.
- Linux: `su_authd` applies the user's config to the recognizer before each
  authentication; the GUI applies it on save, on preview start and before
  enrollment or test captures.

## Integration

`RecognizerService::observe_exposure(frame, face, camera_generation)` runs the
loop and returns whether a camera control changed. It is called at the end of
`capture_and_extract` (the Linux PAM daemon and the Windows recognition agent)
and from the GUI preview inference thread, but not for imported images. The
face box is used even when the result has no usable face (`has_face` false): a
face found only by the low-light retry still gives the loop something to
meter. The ROI follows the face only when it moved more than 10% of the frame
and at least 500 ms passed since the last change. Debug log event
`camera.exposure_observe` is throttled to once per 2 s.

- Locking: the metering region and the luma statistics are computed before
  `camera_mutex_` is taken; the lock is held only to check the session and
  apply the decision.
- Session generation: a counter bumped on every camera open and close is
  stamped on each captured frame (`PreviewFrame::camera_generation`). The loop
  ignores a frame whose generation, width or height differ from the current
  session, so a frame grabbed before a close/reopen cannot steer the new
  session.
- Liveness reset: when a control changed (a level or restore, not a region
  move), the liveness window is restarted (`capture_and_extract` and the
  preview controller call `reset_liveness()`), so liveness never aggregates
  frames across a brightness step.

## Camera API

```cpp
std::expected<void, ExposureWriteError> set_metering_region(PixelRegion);
std::expected<int,  ExposureWriteError> set_exposure_level(ExposureKnobKind, int); // applied level
std::expected<void, ExposureWriteError> reset_exposure_knob(ExposureKnobKind);
```

## Tests

`su_exposure_control_test` (pure loop: runtime and `static_assert`) and
`su_v4l2_exposure_test` (V4L2 controls against a fake device through an
injected ioctl: failed reset retried on close, restore ordering, partial
failure, EINTR, error classification, sparse EV menu, region reset). The
switch is covered by `su_seetaface_pipeline_smoke_test` (retry off: the
darkened face returns no box) and the Windows protocol tests.

## Tuning constants

| Constant | Value | Where |
|---|---|---|
| `target_low` / `target_high` | 90 / 150 | `ExposureTuning` |
| `max_clipped_fraction`, `clip_level` | 5%, 250 | `ExposureTuning` |
| `settle_observations` | 4 skipped observations | `ExposureTuning` |
| `min_settle_time` | 500 ms | `ExposureTuning` |
| `flip_flop_memory` | 30 s | `ExposureTuning` |
| `kMaxTransientFailures` | 3 | `service_exposure.cpp` |
| `kMaxManualExposure` | 333 | `v4l2_exposure.h` |
| `kMaxManualExposureUnits` | 333 | `windows_camera_controls.h` |
| ROI move threshold / interval | 10% / 500 ms | `service_exposure.cpp` |

## Platform matrix

| Platform | Region | Level knob |
|---|---|---|
| Linux V4L2 (UVC) | ROI rect + auto bitmask (if the camera exposes it) | chain: ev bias > backlight compensation > manual exposure |
| Windows Media Foundation | none (`set_metering_region` returns `unsupported`) | chain: backlight compensation > manual exposure |

### Windows

- Backlight compensation: `IAMVideoProcAmp`, `VideoProcAmp_BacklightCompensation`
  (range and step from `GetRange`, written with the manual flag).
- Manual exposure: `IAMCameraControl`, `CameraControl_Exposure`. DirectShow
  stores log2 seconds; the loop works in linear 100 microsecond levels, so
  values are converted to and from log2 seconds on the driver's step grid
  (nearest in the log domain) and capped at 1/30 s. Auto -> manual happens
  only on the first use; leaving manual exposure restores the original flags
  and value, which re-enables auto exposure.
- Every COM call runs on the MF worker thread through `Op::SetExposure`
  (payload: knob kind plus a level, or no level for a reset). Restore happens
  on the worker before the media source is shut down.
- Not implemented: metering ROI and EV compensation through
  `IMFExtendedCameraController`. mingw-w64 lacks the declarations and the
  IIDs, vtable order and payload layouts could not be verified from an
  authoritative source, so there is no metering region and no `ev_bias` knob
  on Windows.
- Verification: compiled for mingw and unit tested (range/unit helpers, chain
  order, the pure loop) under wine only; it has not been run against a real
  Windows camera.
