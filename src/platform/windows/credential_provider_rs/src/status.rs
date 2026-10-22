//! Non-secret display protocol. Keep values in sync with status_host/protocol.h.
use crate::auto_recognition::{AttemptMachine, Outcome, Phase, StopReason};

// Packed word: phase bits 0–3, automatic bit 4, last-no-face bit 5,
// countdown seconds bits 8–23. Bits 6–7 and 24–31 remain reserved.
pub const LAST_NO_FACE: u32 = 1 << 5;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum DisplayState {
    Hidden = 0,
    Waiting = 1,
    InitialDelay = 2,
    Recognizing = 3,
    RetryDelay = 4,
    Ready = 5,
    Submitting = 6,
    NoMatch = 7,
    TimedOut = 8,
    Unavailable = 9,
    LoginFailed = 10,
}

pub fn snapshot(machine: &AttemptMachine, now_ms: u64, visible: bool, login_failed: bool) -> u32 {
    if !visible {
        return DisplayState::Hidden as u32;
    }
    let state = if login_failed {
        DisplayState::LoginFailed
    } else {
        match machine.phase() {
            Phase::Idle => DisplayState::Waiting,
            Phase::InitialDelay => DisplayState::InitialDelay,
            Phase::Recognizing => DisplayState::Recognizing,
            Phase::RetryDelay => DisplayState::RetryDelay,
            Phase::Ready => DisplayState::Ready,
            Phase::Submitted => DisplayState::Submitting,
            Phase::Stopped => match machine.stop_reason() {
                Some(StopReason::TimedOut) => DisplayState::TimedOut,
                Some(StopReason::Unavailable) => DisplayState::Unavailable,
                _ => DisplayState::NoMatch,
            },
        }
    };
    let seconds = if matches!(state, DisplayState::InitialDelay | DisplayState::RetryDelay) {
        machine.delay_remaining_ms(now_ms).div_ceil(1000).min(65535) as u32
    } else {
        0
    };
    let no_face = if machine.retry_hint() == Some(Outcome::NoFace) {
        LAST_NO_FACE
    } else {
        0
    };
    state as u32 | ((machine.settings().is_automatic() as u32) << 4) | no_face | (seconds << 8)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Outcome, Poll, TriggerSettings};

    #[test]
    fn manual_waits_then_reports_one_attempt_and_its_failure() {
        let mut machine = AttemptMachine::new();
        assert_eq!(snapshot(&machine, 0, true, false), 1);
        let (generation, _) = machine.begin_on_demand(0, "test", 1, 1);
        assert!(matches!(machine.poll(0), Poll::Recognize(_)));
        assert_eq!(snapshot(&machine, 0, true, false), 3);
        machine.complete(generation, 100, Outcome::Transient);
        assert_eq!(snapshot(&machine, 100, true, false), 7);
        assert_eq!(snapshot(&machine, 100, false, true), 0);
    }

    #[test]
    fn automatic_countdown_and_retry_follow_real_deadlines() {
        let mut machine = AttemptMachine::with_settings(TriggerSettings::from_values(
            Some(1),
            Some(3),
            Some(5),
            Some(30),
        ));
        let (generation, _) = machine.begin(100, "test", 1, 1);
        assert_eq!(snapshot(&machine, 101, true, false), 2 | 16 | (3 << 8));
        assert_eq!(snapshot(&machine, 2101, true, false), 2 | 16 | (1 << 8));
        machine.poll(3100);
        machine.complete(generation, 4000, Outcome::Transient);
        assert_eq!(snapshot(&machine, 4000, true, false), 4 | 16 | (5 << 8));
        machine.expire(30100);
        assert_eq!(snapshot(&machine, 30100, true, false), 8 | 16);
    }

    #[test]
    fn grant_is_distinct_from_submission_and_windows_rejection() {
        let mut machine = AttemptMachine::new();
        let (generation, _) = machine.begin_on_demand(0, "test", 1, 1);
        machine.poll(0);
        machine.complete(generation, 10, Outcome::Success);
        assert_eq!(snapshot(&machine, 10, true, false), 5);
        assert!(machine.take_ready(generation, 11));
        assert_eq!(snapshot(&machine, 11, true, false), 6);
        assert_eq!(snapshot(&machine, 12, true, true), 10);
        machine.cancel();
        let (generation, _) = machine.begin_on_demand(20, "test", 1, 2);
        machine.poll(20);
        machine.complete(generation, 30, Outcome::Fatal);
        assert_eq!(snapshot(&machine, 30, true, false), 9);
    }

    #[test]
    fn no_face_flag_survives_retry_and_timeout_then_clears_on_new_attempt() {
        let mut machine = AttemptMachine::with_settings(TriggerSettings::from_values(
            Some(1),
            Some(0),
            Some(1),
            Some(5),
        ));
        let (generation, _) = machine.begin(0, "test", 1, 1);
        machine.poll(0);
        machine.complete(generation, 1, Outcome::NoFace);
        assert_eq!(
            snapshot(&machine, 1, true, false),
            4 | 16 | LAST_NO_FACE | (1 << 8)
        );
        machine.poll(1001);
        assert_eq!(snapshot(&machine, 1001, true, false), 3 | 16 | LAST_NO_FACE);
        machine.expire(5000);
        assert_eq!(snapshot(&machine, 5000, true, false), 8 | 16 | LAST_NO_FACE);
        assert_eq!(snapshot(&machine, 5000, false, false), 0);
        machine.begin_on_demand(6000, "test", 1, 2);
        assert_eq!(snapshot(&machine, 6000, true, false) & LAST_NO_FACE, 0);
    }

    #[test]
    fn mismatch_replaces_no_face_flag_and_manual_no_face_stops_with_flag() {
        let mut machine = AttemptMachine::with_settings(TriggerSettings::from_values(
            Some(1),
            Some(0),
            Some(1),
            Some(30),
        ));
        let (generation, _) = machine.begin(0, "test", 1, 1);
        machine.poll(0);
        machine.complete(generation, 1, Outcome::NoFace);
        machine.poll(1001);
        machine.complete(generation, 1002, Outcome::Mismatch);
        assert_eq!(snapshot(&machine, 1002, true, false), 4 | 16 | (1 << 8));
        machine.expire(30_000);
        assert_eq!(snapshot(&machine, 30_000, true, false), 8 | 16);
        machine.set_settings(TriggerSettings::manual());
        let (generation, _) = machine.begin_on_demand(31_000, "test", 1, 2);
        machine.poll(31_000);
        machine.complete(generation, 31_001, Outcome::NoFace);
        assert_eq!(snapshot(&machine, 31_001, true, false), 7 | LAST_NO_FACE);
        machine.cancel();
        assert_eq!(snapshot(&machine, 31_002, true, false), 1);
    }
}
