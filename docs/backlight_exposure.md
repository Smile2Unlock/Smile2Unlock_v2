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
- Anti-oscillation: after every change the loop waits `settle_observations`
  (4) observations before deciding again.
- Normal-light users see no behaviour change: the loop holds. The only
  difference is that the camera meters the face or centre region.

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
      never at open, so the loop must keep tracking the scene. Exposure time
      moves proportionally toward the middle of the band (ratio clamped to
      `[0.5, 2]`, at least one step) and is capped at 1/30 s (333 in 100
      microsecond units) and at the device maximum.

A single knob saturates: backlight compensation is often only 0..1 or 0..2
(the UVC webcam used for the hardware check: 0..2) and a face at luma ~12/255
needs more. So `next_exposure_step` escalates:

- Brighten: step the active knob. When it is at its maximum, escalate to the
  next knob in the chain, starting from its initial level, and step that one.
- Darken (too bright, or clipped): step the highest escalated knob down. Once
  an escalated knob is back at its initial level the loop de-escalates: it emits
  a `restore` for that knob (manual exposure hands control back to the camera's
  auto exposure at its original mode and value) and the previous knob becomes
  active. The first knob of the chain may go below its initial level.
- The state keeps one level per knob kind plus the active (highest escalated)
  kind; settle/hysteresis/clipping rules are unchanged, and nothing is touched
  while the face is in band. An empty chain never acts.
- Each step is one `ExposureChange { kind, set | restore, level }`; the camera
  API is `set_exposure_level(kind, level)` and `reset_exposure_knob(kind)`.
- If a write fails the service drops only that knob from the session chain
  (the loop skips or escalates past it), makes a best-effort reset of it and
  logs `camera.exposure_knob_failed` once.

## Restore guarantee

UVC cameras keep control values in firmware, so changes would leak into other
apps. Every control touched is read at `open()` and restored in reverse chain order
by `release()` and the destructor (and a knob the loop de-escalates out of is
restored immediately): absolute exposure before auto exposure is
re-enabled, and the ROI auto bitmask before the ROI rect. A failed restore is
logged as `camera.exposure_restore_failed`. Probe failures never fail `open()`.

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

## Integration

`RecognizerService::observe_exposure(frame, face)` runs the loop. It is called
at the end of `capture_and_extract` (the Linux PAM daemon and the Windows
recognition agent) and from the GUI preview inference thread, but not for
imported images. The ROI follows the face only when it moved more than 10% of
the frame and at least 500 ms passed since the last change. Debug log event
`camera.exposure_observe` is throttled to once per 2 s.

## Tuning constants

| Constant | Value | Where |
|---|---|---|
| `target_low` / `target_high` | 90 / 150 | `ExposureTuning` |
| `max_clipped_fraction`, `clip_level` | 5%, 250 | `ExposureTuning` |
| `settle_observations` | 4 | `ExposureTuning` |
| `kMaxManualExposure` | 333 | `v4l2_exposure.h` |
| `kMaxManualExposureUnits` | 333 | `windows_camera_controls.h` |
| ROI move threshold / interval | 10% / 500 ms | `service_exposure.cpp` |

## Platform matrix

| Platform | Region | Level knob |
|---|---|---|
| Linux V4L2 (UVC) | ROI rect + auto bitmask (if the camera exposes it) | chain: ev bias > backlight compensation > manual exposure |
| Windows Media Foundation | none (`set_metering_region` returns false) | chain: backlight compensation > manual exposure |

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
