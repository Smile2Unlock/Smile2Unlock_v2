//! Automatic face-recognition trigger logic (pure, platform independent).
//!
//! The Windows Credential Provider must not run recognition on the LogonUI
//! thread and must never block `GetSerialization` on a camera. This module
//! holds the timing and lifecycle rules in one place so they can be tested
//! with a fake clock and a fake transport:
//!
//! - selecting a tile in automatic mode starts one attempt after the initial
//!   delay; repeated selection never starts duplicate work,
//! - transient outcomes (no face, camera busy, no match) are retried after the
//!   retry delay inside a single overall deadline,
//! - hard outcomes (stale password, access denied, missing profile, model
//!   failure) stop the attempt and leave manual password entry available,
//! - deselection, password editing, user-array replacement, `UnAdvise` and
//!   deadline expiry invalidate the attempt; a late completion is ignored.
//!
//! The machine owns no secret. The prepared logon password stays in the
//! caller's protected buffer and is only published after a matching
//! completion. See `auto_runtime` for the Windows worker that drives this
//! machine.

use std::time::Duration;

/// Defaults and bounds mirror `src/core-rs/src/config.rs` (`normalize_config`)
/// and the service-side reader, so the GUI, the service and the provider all
/// agree on what a stored value means.
pub const DEFAULT_AUTO_DELAY_SEC: u32 = 3;
pub const DEFAULT_RETRY_DELAY_SEC: u32 = 5;
pub const DEFAULT_TIMEOUT_SEC: u32 = 30;
pub const MAX_DELAY_SEC: u32 = 3600;
pub const MIN_TIMEOUT_SEC: u32 = 5;
pub const MAX_TIMEOUT_SEC: u32 = 600;
/// Upper bound on recognition attempts inside one attempt, independent of the
/// deadline. Prevents a one-second retry delay from spawning hundreds of
/// camera agents during a long timeout.
pub const MAX_ATTEMPTS: u32 = 12;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TriggerMode {
    Manual,
    Automatic,
}

/// Trigger policy read from the per-SID settings store.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TriggerSettings {
    pub mode: TriggerMode,
    pub auto_delay: Duration,
    pub retry_delay: Duration,
    pub timeout: Duration,
}

impl TriggerSettings {
    pub const fn manual() -> Self {
        Self {
            mode: TriggerMode::Manual,
            auto_delay: Duration::from_secs(DEFAULT_AUTO_DELAY_SEC as u64),
            retry_delay: Duration::from_secs(DEFAULT_RETRY_DELAY_SEC as u64),
            timeout: Duration::from_secs(DEFAULT_TIMEOUT_SEC as u64),
        }
    }

    /// Normalize raw registry values exactly like the shared core config.
    /// `None` means the value is absent and falls back to the default.
    pub fn from_values(
        mode: Option<u32>,
        auto_delay_sec: Option<u32>,
        retry_delay_sec: Option<u32>,
        timeout_sec: Option<u32>,
    ) -> Self {
        let mode = match mode {
            Some(1) => TriggerMode::Automatic,
            _ => TriggerMode::Manual,
        };
        let auto_delay_sec = match auto_delay_sec {
            Some(value) if value <= MAX_DELAY_SEC => value,
            _ => DEFAULT_AUTO_DELAY_SEC,
        };
        let retry_delay_sec = match retry_delay_sec {
            Some(value) if (1..=MAX_DELAY_SEC).contains(&value) => value,
            _ => DEFAULT_RETRY_DELAY_SEC,
        };
        let timeout_sec = match timeout_sec {
            Some(value) if (MIN_TIMEOUT_SEC..=MAX_TIMEOUT_SEC).contains(&value) => value,
            _ => DEFAULT_TIMEOUT_SEC,
        };
        Self {
            mode,
            auto_delay: Duration::from_secs(auto_delay_sec as u64),
            retry_delay: Duration::from_secs(retry_delay_sec as u64),
            timeout: Duration::from_secs(timeout_sec as u64),
        }
    }

    pub fn is_automatic(self) -> bool {
        self.mode == TriggerMode::Automatic
    }

    pub fn auto_delay_ms(self) -> u64 {
        self.auto_delay.as_millis() as u64
    }

    pub fn retry_delay_ms(self) -> u64 {
        self.retry_delay.as_millis() as u64
    }

    pub fn timeout_ms(self) -> u64 {
        self.timeout.as_millis() as u64
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Idle,
    InitialDelay,
    Recognizing,
    RetryDelay,
    Ready,
    Submitted,
    Stopped,
}

/// Work item handed to the transport. `deadline_ms` is the absolute
/// monotonic deadline of the whole attempt, not of this single capture.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RecognitionRequest {
    pub generation: u64,
    pub request_id: u64,
    pub session_id: u32,
    pub deadline_ms: u64,
}

/// What the worker should do next.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Poll {
    /// Nothing to do; wait for an external signal.
    Idle,
    /// Wait until the monotonic timestamp, then poll again.
    WaitUntil(u64),
    /// Run one recognition attempt.
    Recognize(RecognitionRequest),
    /// A grant is published; notify LogonUI once.
    Ready,
    /// The attempt ended without a grant (hard failure or exhausted).
    Finished,
}

/// Outcome reported by the transport for one recognition attempt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// Face matched and the service released the one-time secret.
    Success,
    /// Retryable: no face, camera busy, no match, liveness not met.
    Transient,
    /// Stop: stale/consumed password, access denied, missing profile,
    /// invalid model or unavailable service.
    Fatal,
}

/// Result of completing one attempt against the current generation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Completion {
    /// Grant published; the worker must notify LogonUI.
    Accepted,
    /// Retry scheduled.
    Retry,
    /// Attempt stopped after the last allowed try or the deadline.
    Exhausted,
    /// Attempt stopped for a non-retryable reason.
    Aborted,
    /// The completion belongs to a superseded attempt; ignore it.
    Stale,
}

/// Timing/lifecycle state machine for one selected tile.
///
/// Time is supplied by the caller as monotonic milliseconds. The machine never
/// reads the clock itself, which is what makes the deadline and retry rules
/// testable with a fake clock.
#[derive(Debug)]
pub struct AttemptMachine {
    settings: TriggerSettings,
    phase: Phase,
    generation: u64,
    sid: Option<String>,
    session_id: u32,
    request_id: u64,
    deadline_ms: u64,
    next_ms: u64,
    attempts: u32,
    in_flight: bool,
}

impl AttemptMachine {
    pub fn new() -> Self {
        Self::with_settings(TriggerSettings::manual())
    }

    pub fn with_settings(settings: TriggerSettings) -> Self {
        Self {
            settings,
            phase: Phase::Idle,
            generation: 0,
            sid: None,
            session_id: 0,
            request_id: 0,
            deadline_ms: 0,
            next_ms: 0,
            attempts: 0,
            in_flight: false,
        }
    }

    pub fn set_settings(&mut self, settings: TriggerSettings) {
        self.settings = settings;
    }

    pub fn settings(&self) -> TriggerSettings {
        self.settings
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// SID of the attempt currently in progress, if any.
    pub fn active_sid(&self) -> Option<&str> {
        if self.is_active() {
            self.sid.as_deref()
        } else {
            None
        }
    }

    /// True while a selected automatic attempt still owns the tile.
    pub fn is_active(&self) -> bool {
        matches!(
            self.phase,
            Phase::InitialDelay | Phase::Recognizing | Phase::RetryDelay | Phase::Ready
        )
    }

    /// Start (or re-arm) an automatic attempt for `sid`.
    ///
    /// Returns `(generation, started)`. `started` is false when the mode is
    /// manual or when the same user is already mid-attempt, so a duplicated
    /// `SetSelected` cannot start a second camera agent.
    pub fn begin(
        &mut self,
        now_ms: u64,
        sid: &str,
        session_id: u32,
        request_id: u64,
    ) -> (u64, bool) {
        if !self.settings.is_automatic() {
            return (self.generation, false);
        }
        if self.is_active() && self.sid.as_deref() == Some(sid) {
            return (self.generation, false);
        }
        self.generation = self.generation.wrapping_add(1);
        self.phase = Phase::InitialDelay;
        self.sid = Some(sid.to_owned());
        self.session_id = session_id;
        self.request_id = request_id;
        self.deadline_ms = now_ms.saturating_add(self.settings.timeout_ms());
        self.next_ms = now_ms.saturating_add(self.settings.auto_delay_ms());
        self.attempts = 0;
        self.in_flight = false;
        (self.generation, true)
    }

    /// Invalidate the attempt. Any completion carrying an older generation is
    /// ignored, and any grant the caller holds must be erased.
    pub fn cancel(&mut self) -> u64 {
        self.generation = self.generation.wrapping_add(1);
        self.phase = Phase::Idle;
        self.sid = None;
        self.session_id = 0;
        self.request_id = 0;
        self.deadline_ms = 0;
        self.next_ms = 0;
        self.attempts = 0;
        self.in_flight = false;
        self.generation
    }

    fn request(&self) -> RecognitionRequest {
        RecognitionRequest {
            generation: self.generation,
            request_id: self.request_id,
            session_id: self.session_id,
            deadline_ms: self.deadline_ms,
        }
    }

    fn begin_capture(&mut self) {
        self.phase = Phase::Recognizing;
        self.attempts = self.attempts.saturating_add(1);
        self.in_flight = true;
    }

    /// Ask what to do next. Must be called with the current monotonic time.
    pub fn poll(&mut self, now_ms: u64) -> Poll {
        match self.phase {
            Phase::Idle | Phase::Submitted | Phase::Stopped => Poll::Idle,
            Phase::Ready => Poll::Ready,
            Phase::InitialDelay | Phase::RetryDelay => {
                if now_ms >= self.deadline_ms {
                    self.phase = Phase::Stopped;
                    self.in_flight = false;
                    return Poll::Finished;
                }
                if now_ms < self.next_ms {
                    return Poll::WaitUntil(self.next_ms);
                }
                if self.attempts >= MAX_ATTEMPTS {
                    self.phase = Phase::Stopped;
                    self.in_flight = false;
                    return Poll::Finished;
                }
                self.begin_capture();
                Poll::Recognize(self.request())
            }
            Phase::Recognizing => {
                if now_ms >= self.deadline_ms {
                    // The blocking call is cancelled by the runtime; stop
                    // scheduling further attempts.
                    self.phase = Phase::Stopped;
                    self.in_flight = false;
                    return Poll::Finished;
                }
                // A capture is in flight on the worker; do not start another.
                Poll::WaitUntil(self.deadline_ms)
            }
        }
    }

    /// Record the outcome of the capture identified by `generation`.
    pub fn complete(&mut self, generation: u64, now_ms: u64, outcome: Outcome) -> Completion {
        if generation != self.generation {
            return Completion::Stale;
        }
        self.in_flight = false;
        match outcome {
            Outcome::Success => {
                // A grant that arrives after the attempt deadline is not
                // published: the user may have stopped waiting or reselected.
                if now_ms > self.deadline_ms {
                    self.phase = Phase::Stopped;
                    return Completion::Exhausted;
                }
                self.phase = Phase::Ready;
                Completion::Accepted
            }
            Outcome::Fatal => {
                self.phase = Phase::Stopped;
                Completion::Aborted
            }
            Outcome::Transient => {
                let next = now_ms.saturating_add(self.settings.retry_delay_ms());
                if self.attempts >= MAX_ATTEMPTS || next > self.deadline_ms {
                    self.phase = Phase::Stopped;
                    Completion::Exhausted
                } else {
                    self.phase = Phase::RetryDelay;
                    self.next_ms = next;
                    Completion::Retry
                }
            }
        }
    }

    /// Consume the published grant exactly once. Returns false for a stale
    /// generation or when no grant is ready, so `GetSerialization` can never
    /// serialize the same face grant twice.
    pub fn take_ready(&mut self, generation: u64) -> bool {
        if generation == self.generation && self.phase == Phase::Ready {
            self.phase = Phase::Submitted;
            true
        } else {
            false
        }
    }
}

impl Default for AttemptMachine {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const AUTO: TriggerSettings = TriggerSettings {
        mode: TriggerMode::Automatic,
        auto_delay: Duration::from_secs(3),
        retry_delay: Duration::from_secs(5),
        timeout: Duration::from_secs(30),
    };

    fn machine() -> AttemptMachine {
        AttemptMachine::with_settings(AUTO)
    }

    #[test]
    fn settings_normalization_matches_shared_core() {
        assert_eq!(
            TriggerSettings::from_values(None, None, None, None),
            TriggerSettings::manual()
        );
        assert_eq!(
            TriggerSettings::from_values(Some(9), Some(99_999), Some(0), Some(1)),
            TriggerSettings::manual()
        );
        let auto = TriggerSettings::from_values(Some(1), Some(0), Some(1), Some(600));
        assert!(auto.is_automatic());
        assert_eq!(auto.auto_delay_ms(), 0);
        assert_eq!(auto.retry_delay_ms(), 1_000);
        assert_eq!(auto.timeout_ms(), 600_000);
        // Bounds are inclusive at the edge and fall back outside them.
        assert_eq!(
            TriggerSettings::from_values(Some(1), Some(3600), Some(3600), Some(5)).timeout_ms(),
            5_000
        );
        assert_eq!(
            TriggerSettings::from_values(Some(1), Some(3601), Some(3601), Some(601))
                .auto_delay_ms(),
            DEFAULT_AUTO_DELAY_SEC as u64 * 1000
        );
    }

    #[test]
    fn manual_mode_never_starts_work() {
        let mut machine = AttemptMachine::with_settings(TriggerSettings::manual());
        let (generation, started) = machine.begin(0, "S-1-5-21-1", 1, 7);
        assert!(!started);
        assert_eq!(generation, 0);
        assert_eq!(machine.phase(), Phase::Idle);
        assert_eq!(machine.poll(10_000), Poll::Idle);
    }

    #[test]
    fn zero_initial_delay_recognizes_immediately() {
        let mut machine = AttemptMachine::with_settings(TriggerSettings {
            auto_delay: Duration::ZERO,
            ..AUTO
        });
        let (generation, started) = machine.begin(1_000, "S-1-5-21-1", 2, 42);
        assert!(started);
        match machine.poll(1_000) {
            Poll::Recognize(request) => {
                assert_eq!(request.generation, generation);
                assert_eq!(request.request_id, 42);
                assert_eq!(request.session_id, 2);
                assert_eq!(request.deadline_ms, 31_000);
            }
            other => panic!("expected Recognize, got {other:?}"),
        }
        // A second poll while the capture is in flight must not start another.
        assert_eq!(machine.poll(1_100), Poll::WaitUntil(31_000));
    }

    #[test]
    fn repeated_selection_does_not_start_duplicate_work() {
        let mut machine = machine();
        let (first, started) = machine.begin(0, "S-1-5-21-1", 1, 1);
        assert!(started);
        let (second, started) = machine.begin(500, "S-1-5-21-1", 1, 2);
        assert!(!started);
        assert_eq!(first, second);
        assert_eq!(machine.poll(0), Poll::WaitUntil(3_000));
        // A different user replaces the attempt.
        let (third, started) = machine.begin(1_000, "S-1-5-21-2", 1, 3);
        assert!(started);
        assert_ne!(first, third);
    }

    #[test]
    fn initial_delay_then_recognize() {
        let mut machine = machine();
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        assert_eq!(machine.poll(2_999), Poll::WaitUntil(3_000));
        match machine.poll(3_000) {
            Poll::Recognize(request) => assert_eq!(request.generation, generation),
            other => panic!("expected Recognize, got {other:?}"),
        }
    }

    #[test]
    fn transient_outcome_retries_after_retry_delay() {
        let mut machine = machine();
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        assert_eq!(
            machine.complete(generation, 4_000, Outcome::Transient),
            Completion::Retry
        );
        assert_eq!(machine.phase(), Phase::RetryDelay);
        assert_eq!(machine.poll(8_999), Poll::WaitUntil(9_000));
        match machine.poll(9_000) {
            Poll::Recognize(_) => {}
            other => panic!("expected Recognize, got {other:?}"),
        }
    }

    #[test]
    fn success_publishes_once_and_serializes_once() {
        let mut machine = machine();
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        assert_eq!(
            machine.complete(generation, 3_500, Outcome::Success),
            Completion::Accepted
        );
        assert_eq!(machine.phase(), Phase::Ready);
        assert_eq!(machine.poll(4_000), Poll::Ready);
        assert!(machine.take_ready(generation));
        assert_eq!(machine.phase(), Phase::Submitted);
        assert!(!machine.take_ready(generation), "grant must be single-use");
        assert_eq!(machine.poll(5_000), Poll::Idle);
    }

    #[test]
    fn success_after_deadline_is_discarded() {
        let mut machine = machine();
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        // The capture returned one millisecond after the 30 s deadline.
        assert_eq!(
            machine.complete(generation, 30_001, Outcome::Success),
            Completion::Exhausted
        );
        assert_eq!(machine.phase(), Phase::Stopped);
        assert_eq!(machine.poll(30_002), Poll::Idle);
    }

    #[test]
    fn exact_deadline_stops_scheduling() {
        let mut machine = machine();
        machine.begin(0, "S-1-5-21-1", 1, 1);
        // At the deadline boundary a pending retry stops instead of firing.
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        assert_eq!(
            machine.complete(generation, 20_000, Outcome::Transient),
            Completion::Retry
        );
        assert_eq!(machine.poll(30_000), Poll::Finished);
        assert_eq!(machine.phase(), Phase::Stopped);
        assert_eq!(machine.poll(30_001), Poll::Idle);
    }

    #[test]
    fn transient_attempts_are_capped_independently_of_the_deadline() {
        // A one-second retry delay with a ten-minute timeout must still stop at
        // MAX_ATTEMPTS instead of spawning a camera agent every second.
        let mut machine = AttemptMachine::with_settings(TriggerSettings {
            auto_delay: Duration::ZERO,
            retry_delay: Duration::from_secs(1),
            timeout: Duration::from_secs(600),
            ..AUTO
        });
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let mut now = 0_u64;
        let mut completions = 0_u32;
        loop {
            match machine.poll(now) {
                Poll::Recognize(_) => {}
                other => panic!("expected Recognize at {now}, got {other:?}"),
            }
            let completion = machine.complete(generation, now, Outcome::Transient);
            completions += 1;
            if completion == Completion::Exhausted {
                break;
            }
            assert_eq!(completion, Completion::Retry);
            assert!(completions <= MAX_ATTEMPTS, "attempt cap was not enforced");
            now = machine.next_ms;
        }
        assert_eq!(completions, MAX_ATTEMPTS);
        assert_eq!(machine.phase(), Phase::Stopped);
    }

    #[test]
    fn fatal_outcome_stops_immediately() {
        let mut machine = machine();
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        assert_eq!(
            machine.complete(generation, 3_000, Outcome::Fatal),
            Completion::Aborted
        );
        assert_eq!(machine.phase(), Phase::Stopped);
        assert_eq!(machine.poll(3_001), Poll::Idle);
    }

    #[test]
    fn deselection_during_every_phase_ignores_late_completion() {
        for advance in [0_u64, 1_000, 3_000, 4_000, 9_000] {
            let mut machine = machine();
            let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
            if advance >= 3_000 {
                let _ = machine.poll(3_000);
            }
            let cancelled = machine.cancel();
            assert_ne!(cancelled, generation);
            assert_eq!(machine.phase(), Phase::Idle);
            assert_eq!(
                machine.complete(generation, advance + 1_000, Outcome::Success),
                Completion::Stale
            );
            assert!(!machine.take_ready(generation));
            assert_eq!(machine.poll(advance + 2_000), Poll::Idle);
        }
    }

    #[test]
    fn stale_completion_after_reselection_is_ignored() {
        let mut machine = machine();
        let (first, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        machine.cancel();
        let (second, started) = machine.begin(10_000, "S-1-5-21-1", 1, 2);
        assert!(started);
        assert_ne!(first, second);
        assert_eq!(
            machine.complete(first, 11_000, Outcome::Success),
            Completion::Stale
        );
        assert_eq!(machine.phase(), Phase::InitialDelay);
        assert!(!machine.take_ready(first));
        assert!(!machine.take_ready(second));
    }

    #[test]
    fn reselection_after_hard_failure_can_restart() {
        let mut machine = machine();
        let (generation, _) = machine.begin(0, "S-1-5-21-1", 1, 1);
        let _ = machine.poll(3_000);
        assert_eq!(
            machine.complete(generation, 3_000, Outcome::Fatal),
            Completion::Aborted
        );
        let (next, started) = machine.begin(4_000, "S-1-5-21-1", 1, 2);
        assert!(started);
        assert_ne!(generation, next);
    }
}
