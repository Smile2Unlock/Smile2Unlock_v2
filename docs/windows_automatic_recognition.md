# Windows automatic recognition: implementation design

Status: design reviewed against the current code, not implemented. The automatic
mode, initial delay, retry delay and timeout controls remain hidden. Manual face
submission and manual password submission remain the supported login paths.

## Current gaps

- `CoreConfig` persists `recognition_mode`, `auto_delay_sec`, `retry_delay_sec`
  and `timeout_sec`, but the Credential Provider does not read them.
- `Provider::Advise` discards the events interface; `event_sink.rs` is a skeleton.
  No worker can currently notify LogonUI that face authentication completed.
- `GetCredentialAt` creates a new credential on every call. It also calls
  `SetSelected` during a diagnostic self-test. Adding a timer to `SetSelected`
  alone would start recognition during enumeration and lose state on requery.
- `GetSerialization` blocks in `CallNamedPipeW`. Its 3000 ms argument controls
  waiting for an available pipe, not the duration of the complete transaction.
  Changing that argument does not implement an authentication deadline.
- The service serializes requests and launches an agent with a fixed 10 second
  timeout. The agent accepts at most 30 seconds, while config accepts up to 600.
  It returns the first live embedding; the service compares it only once.

## Proposed lifecycle

Keep stable credentials indexed by SID for the current user-array generation.
Remove the diagnostic `SetSelected` invocation before adding selection effects.
Store the provider's events interface in the COM Global Interface Table, obtaining
a proxy from a COM-initialized worker apartment. Do not mark raw COM interfaces
`Send` or `Sync`. Retain the DLL and worker state until the worker exits.

Use a mutex-protected attempt state with SID, session ID, generation, deadline,
cancellation event and an optional protected prepared password. The states are
idle, initial delay, recognizing, retry delay, ready, submitted and stopped.

1. Selection in automatic mode starts the initial delay. Selection in manual
   mode does not start a worker. Repeated selection must not start duplicate work.
2. The worker performs a fresh service-owned recognition request after the delay.
   Neither the GUI nor the CP may assert that an embedding was authenticated.
3. On success, publish the protected password only if selection, generation and
   deadline still match. Invoke `CredentialsChanged` through the marshaled proxy.
   `GetCredentialCount` supplies the ready credential index and enables autologon;
   `GetSerialization` atomically consumes the prepared result exactly once.
4. Retry transient camera/no-face/no-match outcomes after the retry delay, within
   one overall deadline. Stale password, access denied, absent profiles and model
   failure stop the attempt and preserve manual password entry.
5. Deselection, password editing, user-array replacement, UnAdvise and timeout
   increment the generation, cancel outstanding I/O and erase prepared secrets.
   A late result is erased without notifying LogonUI or submitting credentials.
6. Failed Windows logon stops automatic retries. In particular, a stale stored
   password must not repeatedly attempt logon and lock the Windows account.

The UI thread must never join a blocking worker. Worker ownership and module
lifetime must allow cancellation and eventual cleanup after UI teardown.

## Service and configuration changes required

Persist bounded trigger settings in the existing per-SID registry location and
load them for the selected SID, never LogonUI's SYSTEM account. Define how cold
logon works when that user's registry hive is not loaded: the current registry
lookup otherwise silently falls back to defaults. A service-owned settings API
and store is preferable before claiming automatic mode works at cold boot.

Add bounded attempt/deadline and cancellation semantics to the authenticated IPC
contract. Use overlapped pipe I/O with an absolute deadline; closing or cancelling
the client must also terminate its camera agent. The service needs a bounded
worker model so a long recognition attempt cannot block settings, manual requests
or other sessions. Keep face matching in the service. Retry capture and matching
within the attempt, not just capture of the first live face.

Config bounds, CP deadline, service deadline, agent timeout and UI text must agree.
No UI controls should be restored until all of these consumers are connected.

## Acceptance tests

- Fake-clock state tests: zero initial delay, retry delay, exact deadline, repeated
  selection, deselection during every phase and stale completion after reselection.
- Fake transport: timeout, cancellation, wrong SID/session/request ID, invalid
  model, no profile, mismatch followed by a match and broker password rejection.
- COM integration: stable identity after `CredentialsChanged`, no recognition
  during enumeration, one serialization per result, event revocation and DLL
  lifetime while an operation is being cancelled.
- Real Windows secure desktop: cold boot, lock/unlock, switching users, RDP,
  password typing during countdown, unplugged camera, missing model, expired
  password and service restart. Wine cannot establish these behaviors.

## SeetaFace audit in the accompanying change

The SDK still performs detection, landmarks, embeddings and per-frame anti-spoof
inference. The project aggregates exactly ten finite, sufficiently clear frame
scores, avoiding upstream PredictVideo's N+1/N average and missing clarity gate.
No-face, multiple-face, invalid-image and disabled-liveness calls reset the window.
Grayscale and RGBA inputs are normalized to the SDK's three-channel RGB contract.
The Windows agent skips anti-spoof inference when disabled and reports unavailable
models immediately. Release-mode smoke tests now retain their assertions. PNG and
JPEG decoding is linked into CImg rather than relying on an external ImageMagick
installation; the Wine pipeline reproduced PNG import failure without this fix.

The ten-frame window is not identity tracking: an instantaneous face substitution
without a no-face/multiple-face frame is not detected by the reset rule alone.
Photo/video attack resistance and live camera usability require physical tests;
passing an embedding smoke test is not evidence of resistance to spoofing.
