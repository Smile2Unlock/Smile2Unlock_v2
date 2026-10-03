//! Power transitions, independent of Win32 and authentication. A return is
//! permission to consider a fresh attempt, never proof of identity.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PowerEvent {
    Suspend,
    ResumeAutomatic,
    ResumeInteractive,
    Lid(bool),    // true = open
    Display(u32), // 0 = off, 1 = on, 2 = dim
}

#[derive(Debug, Default)]
pub(crate) struct WakePolicy {
    suspended: bool,
    lid: Option<bool>,
    display: Option<u32>,
    display_was_off: bool,
    away: bool,
    returned: bool,
    orphan_resume_used: bool,
    cycle: u64,
}

impl WakePolicy {
    pub(crate) fn blocked(&self) -> bool {
        self.suspended || self.lid == Some(false) || self.display == Some(0)
    }

    fn leave(&mut self) {
        if !self.away {
            self.cycle = self.cycle.wrapping_add(1);
            self.away = true;
            self.returned = false;
            self.orphan_resume_used = false;
        }
    }

    /// Returns true when outstanding work must be invalidated immediately.
    /// Initial closed/off samples pause work but never count as a return.
    pub(crate) fn observe(&mut self, event: PowerEvent) -> bool {
        let pause = match event {
            PowerEvent::Suspend => {
                self.suspended = true;
                self.leave();
                true
            }
            PowerEvent::ResumeAutomatic => {
                self.suspended = false;
                false
            }
            PowerEvent::ResumeInteractive => {
                self.suspended = false;
                if self.away || !self.orphan_resume_used {
                    self.leave();
                    self.returned = true;
                }
                false
            }
            PowerEvent::Lid(open) => {
                let previous = self.lid.replace(open);
                if !open {
                    self.leave();
                } else if previous == Some(false) {
                    self.returned = true;
                }
                !open && previous != Some(false)
            }
            PowerEvent::Display(value) if value <= 2 => {
                let previous = self.display.replace(value);
                if value == 0 {
                    self.display_was_off = true;
                    self.leave();
                } else if value == 1 && self.display_was_off {
                    self.display_was_off = false;
                    self.returned = true;
                }
                value == 0 && previous != Some(0)
            }
            PowerEvent::Display(_) => false,
        };
        pause
    }

    pub(crate) fn pending(&self) -> Option<u64> {
        (self.away && self.returned && !self.blocked()).then_some(self.cycle)
    }

    pub(crate) fn consume(&mut self) {
        self.away = false;
        self.returned = false;
        self.orphan_resume_used = true;
    }
}

#[cfg(test)]
mod tests {
    use super::{PowerEvent::*, *};

    #[test]
    fn baseline_and_dimming_do_not_create_returns() {
        let mut p = WakePolicy::default();
        for event in [
            Lid(true),
            Display(1),
            Display(2),
            Display(1),
            ResumeAutomatic,
        ] {
            assert!(!p.observe(event));
            assert_eq!(p.pending(), None);
        }
    }

    #[test]
    fn sleeping_laptop_waits_for_interactive_return_and_merges_sources() {
        let mut p = WakePolicy::default();
        p.observe(Lid(true));
        p.observe(Display(1));
        for event in [Lid(false), Display(0), Suspend] {
            assert!(p.observe(event));
        }
        p.observe(ResumeAutomatic);
        assert_eq!(p.pending(), None);
        p.observe(ResumeInteractive);
        p.observe(Lid(true));
        assert_eq!(p.pending(), None);
        p.observe(Display(1));
        let first = p.pending().unwrap();
        p.consume();
        for event in [ResumeInteractive, Display(1), Lid(true), ResumeAutomatic] {
            p.observe(event);
            assert_eq!(p.pending(), None);
        }
        p.observe(Lid(false));
        p.observe(Lid(true));
        assert_ne!(p.pending(), Some(first));
        assert!(p.pending().is_some());
    }

    #[test]
    fn initial_off_blocks_and_actual_on_returns_without_lid_sensor() {
        let mut p = WakePolicy::default();
        assert!(p.observe(Display(0)));
        assert!(p.blocked());
        assert_eq!(p.pending(), None);
        p.observe(Display(1));
        assert!(p.pending().is_some());
    }

    #[test]
    fn off_dim_on_retains_return_but_dim_on_alone_does_not() {
        let mut p = WakePolicy::default();
        p.observe(Display(1));
        p.observe(Display(0));
        p.observe(Display(2));
        assert_eq!(p.pending(), None);
        p.observe(Display(1));
        assert!(p.pending().is_some());
        p.consume();
        p.observe(Display(2));
        p.observe(Display(1));
        assert_eq!(p.pending(), None);
    }

    #[test]
    fn missing_suspend_allows_only_one_interactive_candidate() {
        let mut p = WakePolicy::default();
        p.observe(ResumeInteractive);
        assert!(p.pending().is_some());
        p.consume();
        p.observe(ResumeInteractive);
        assert_eq!(p.pending(), None);
        p.observe(Suspend);
        p.observe(ResumeAutomatic);
        assert_eq!(p.pending(), None);
        p.observe(ResumeInteractive);
        assert!(p.pending().is_some());
    }
}
