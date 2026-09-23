# Windows automatic recognition: implementation design

Status: implemented in the Rust credential provider, the LocalSystem service
and the GUI; real Windows secure-desktop acceptance is still pending. Manual
face submission and manual password submission remain supported and are the
fallback whenever automatic recognition is disabled or cannot complete.

## What is implemented

- `auto_recognition.rs` holds the trigger policy and the attempt state machine
  (idle, initial delay, recognizing, retry delay, ready, submitted, stopped)
  with generation-based cancellation. It is platform independent and tested
  with a fake clock: zero delay, retry delay, exact deadline, repeated
  selection, deselection in every phase, stale completion after reselection,
  the per-attempt cap and single-use grant consumption.
- `auto_runtime.rs` owns the worker thread. `Provider::Advise` registers the
  events interface in the process-wide Global Interface Table; the worker
  obtains a marshaled `ICredentialProviderEvents` proxy and calls
  `CredentialsChanged`. No raw COM pointer crosses apartments and no
  `Send`/`Sync` bypass is used.
- `Provider::GetCredentialCount` returns the ready tile index and enables
  autologon only while a grant is published. `Credential::GetSerialization`
  consumes the prepared password exactly once; the diagnostic `SetSelected`
  call during enumeration was removed.
- Deselection, password editing, user-array replacement, `UnAdvise`, hard
  failures, a failed Windows logon and deadline expiry invalidate the attempt
  and erase the prepared secret. Cancellation calls `CancelSynchronousIo` on
  the worker so the LogonUI thread never waits for a camera.
- The GUI exposes the automatic-mode switch plus initial delay, retry delay
  and timeout. Saving pushes the policy to the service, which persists it
  machine-wide under
  `HKLM\SOFTWARE\Smile2Unlock\Recognition\<SID>`; the provider reads that key
  first (cold-boot safe) and falls back to `HKEY_USERS\<SID>` and defaults.
- The service maps a bounded per-capture agent timeout from the same
  `TimeoutSec` value (clamped to 5–30 s) while the provider owns the overall
  attempt deadline, so one slow capture cannot consume the whole budget.
- The service dispatches accepted transactions to a bounded worker pool
  (`request_worker_pool.h`: 4 workers, queue depth 8, drain-on-stop), so a
  recognition attempt no longer blocks settings writes, manual requests or
  other sessions on the accept loop. When the pool is saturated the caller is
  rejected with `kUnavailable` instead of queueing. Secret-store and
  profile-store mutations are serialized by a mutex held only around the file
  operations; the long agent run stays outside the lock, and
  `ManagementAuthorizer` serializes itself internally.

## Remaining gaps

- The blocking pipe transaction is cancelled with `CancelSynchronousIo`
  instead of overlapped pipe I/O. If that call fails, the worker stays blocked
  until the service responds, then discards the stale result.
- Real Windows secure-desktop acceptance (cold boot, lock/unlock, switching
  users, RDP, unplugged camera, missing model, expired password, service
  restart) has not been run. Wine cannot establish these behaviors.

## Lifecycle

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

## Service and configuration changes

The trigger policy is persisted machine-wide by the service under
`HKLM\SOFTWARE\Smile2Unlock\Recognition\<SID>` (operation
`kSetRecognitionSettings`). The GUI pushes the bounded values when the user
saves settings; the service validates the ranges and writes the key as SYSTEM.
The provider reads that key first, so automatic mode is available at cold boot
before the user hive is loaded, and falls back to `HKEY_USERS\<SID>` for
installs that predate the service store. Never read LogonUI's SYSTEM account.

The service derives the per-capture agent timeout from the same `TimeoutSec`
value, clamped to 5–30 s, while the provider owns the overall attempt deadline.
Config bounds, provider deadline, service agent timeout and UI text therefore
agree: the GUI accepts 0–3600 s delays, 1–3600 s retry delay and 5–600 s
timeout, and the shared Rust core normalizes the same way.

The accept loop hands each validated transaction to a bounded worker pool
(4 workers, queue depth 8) and immediately creates the next pipe instance, so
a long recognition attempt cannot block settings, manual requests or other
sessions. Store and profile mutations share a mutex that the agent run never
holds; a saturated pool rejects with `kUnavailable` rather than queueing;
shutdown drains accepted transactions because each task closes its own pipe.

## Acceptance tests

- Fake-clock state tests: zero initial delay, retry delay, exact deadline, repeated
  selection, deselection during every phase and stale completion after reselection.
  Implemented in `auto_recognition.rs` (runs on Linux and under Wine).
- Fake transport: timeout, cancellation, wrong SID/session/request ID, invalid
  model, no profile, mismatch followed by a match and broker password rejection.
  Retry, cancellation and single-use grant consumption are covered in
  `auto_runtime.rs`; the service-side mapping is covered by `classify` tests.
- COM integration: stable identity after `CredentialsChanged`, no recognition
  during enumeration, one serialization per result, event revocation and DLL
  lifetime while an operation is being cancelled. Enumeration and autologon are
  covered by `provider.rs` tests; the GIT notification path and serialization of
  a real grant still need a real LogonUI run.
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
