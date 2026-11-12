# Backlight-aware exposure (issue #137)

## Problem

With a bright window behind the user, the camera's auto exposure meters the
whole frame and the face comes out nearly black (measured face luma about
12/255 against about 193 for the window), so face recognition fails.

## Approach

A small closed loop meters the face instead of the frame and nudges one camera
control until the face sits in a usable band. The window is allowed to blow
out; only the face matters.

- Region: the inner 70% of the face box once a face is found, otherwise a
  centred window (40% x 50% of the frame) where a seated user's face normally is.
- Target: mean face luma within `[90, 150]` (`ExposureTuning`). Too dark steps
  up; too bright, or more than 5% of face samples clipped (`>= 250`), steps
  down; inside the band the loop holds and touches nothing.
- Anti-oscillation: after every change the loop waits `settle_observations`
  (4) observations before deciding again.
- Normal-light users see no behaviour change: the loop holds. The only
  difference is that the camera meters the face or centre region.

The decision logic is the pure, constexpr function `next_exposure_step` in
`src/recognizer/exposure/exposure_control.h`, unit tested by
`su_exposure_control_test`.

## Mechanism priority

1. Metering region of interest (the camera's own auto exposure keeps running).
2. Plus one level knob, first available of:
   1. `V4L2_CID_AUTO_EXPOSURE_BIAS` (skipped if disabled or inactive)
   2. `V4L2_CID_BACKLIGHT_COMPENSATION`
   3. Manual exposure (`V4L2_CID_EXPOSURE_AUTO` -> manual plus
      `V4L2_CID_EXPOSURE_ABSOLUTE`). Last resort: auto exposure is switched
      off only at the first level change, never at open, so the loop must keep
      tracking the scene. Exposure time moves proportionally toward the middle
      of the band (ratio clamped to `[0.5, 2]`, at least one step) and is
      capped at 1/30 s (333 in 100 microsecond units) and at the device maximum.

## Restore guarantee

UVC cameras keep control values in firmware, so changes would leak into other
apps. Every control touched is read at `open()` and restored in reverse order
by `release()` and the destructor: absolute exposure before auto exposure is
re-enabled, and the ROI auto bitmask before the ROI rect. A failed restore is
logged as `camera.exposure_restore_failed`. Probe failures never fail `open()`.
If setting a level fails, the knob is disabled for that session.

Hardware check (UVC "USB2.0 HD UVC WebCam", backlight_compensation + ROI):
`v4l2-ctl --list-ctrls` is identical before and after a session; while open,
the ROI rect, ROI auto bit and backlight_compensation were observed changed.

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
| ROI move threshold / interval | 10% / 500 ms | `service_exposure.cpp` |

## Platform matrix

| Platform | Region | Level knob |
|---|---|---|
| Linux V4L2 (UVC) | ROI rect + auto bitmask (if the camera exposes it) | ev bias > backlight compensation > manual exposure |
| Windows Media Foundation | TODO (filled in by the Windows backend change) | TODO |
