//! Callback registrations live on a dedicated thread. UnAdvise never waits
//! for unregister or a callback, and epoch checks reject old notifications.
use crate::auto_runtime::{AutoRuntime, PipeRecognitionTransport};
use crate::wake_policy::PowerEvent;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock, Weak, mpsc};
use windows::Win32::Foundation::{HANDLE, HMODULE};
use windows::Win32::System::LibraryLoader::{
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_PIN, GetModuleHandleExW,
};
use windows::Win32::System::Power::{
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, HPOWERNOTIFY, POWERBROADCAST_SETTING,
    PowerRegisterSuspendResumeNotification, PowerSettingRegisterNotification,
    PowerSettingUnregisterNotification, PowerUnregisterSuspendResumeNotification,
};
use windows::Win32::System::RemoteDesktop::{
    WTS_SESSIONSTATE_LOCK, WTSActive, WTSFreeMemory, WTSGetActiveConsoleSessionId, WTSINFOEXW,
    WTSQuerySessionInformationW, WTSSessionInfoEx,
};
use windows::Win32::System::SystemServices::{
    GUID_LIDSWITCH_STATE_CHANGE, GUID_SESSION_DISPLAY_STATUS,
};
use windows::Win32::UI::WindowsAndMessaging::{
    DEVICE_NOTIFY_CALLBACK, PBT_APMRESUMEAUTOMATIC, PBT_APMRESUMESUSPEND, PBT_APMSUSPEND,
    PBT_POWERSETTINGCHANGE,
};
use windows_core::{PCWSTR, PWSTR};

type Runtime = AutoRuntime<PipeRecognitionTransport>;
struct Context {
    runtime: Weak<Runtime>,
    epoch: u64,
    display_baseline: mpsc::Sender<()>,
}

// OS Context is an opaque, never-reused token, not a dereferenced pointer.
// Revoking it prevents late entry after unregister; a callback already in
// progress holds its own Arc. No undocumented callback-drain guarantee needed.
static NEXT_TOKEN: AtomicUsize = AtomicUsize::new(1);
fn contexts() -> &'static Mutex<HashMap<usize, Arc<Context>>> {
    static CONTEXTS: OnceLock<Mutex<HashMap<usize, Arc<Context>>>> = OnceLock::new();
    CONTEXTS.get_or_init(|| Mutex::new(HashMap::new()))
}

pub(crate) struct PowerEvents(mpsc::Sender<()>);
impl PowerEvents {
    pub(crate) fn start(runtime: &Arc<Runtime>) -> Option<Self> {
        // Test/probe hosts must never register with the real power manager.
        if cfg!(test)
            || !std::env::current_exe()
                .ok()?
                .file_name()?
                .eq_ignore_ascii_case("LogonUI.exe")
        {
            return None;
        }
        // Match the DLL's conservative DllCanUnloadNow policy. In particular,
        // a failed unregister must never leave Windows pointing at unloaded code.
        let mut module = HMODULE::default();
        if unsafe {
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                PCWSTR(power_callback as *const () as *const u16),
                &mut module,
            )
        }
        .is_err()
        {
            crate::log::cp_log("power registration: cannot pin module");
            return None;
        }
        let token = NEXT_TOKEN
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
            .ok()?;
        let epoch = runtime.begin_power_subscription();
        let weak = Arc::downgrade(runtime);
        let (stop, receiver) = mpsc::channel();
        let spawned = std::thread::Builder::new()
            .name("su-power-events".into())
            .spawn(move || {
                let (baseline, baseline_received) = mpsc::channel();
                let context = Arc::new(Context {
                    runtime: weak,
                    epoch,
                    display_baseline: baseline,
                });
                contexts()
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .insert(token, Arc::clone(&context));
                // Keep registration parameters stable for their OS lifetime.
                let parameters = Box::new(DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
                    Callback: Some(power_callback),
                    Context: token as *mut c_void,
                });
                let recipient = HANDLE(
                    (&*parameters as *const DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS)
                        .cast_mut()
                        .cast(),
                );
                let mut registrations = Vec::new();
                for guid in [
                    None,
                    Some(GUID_LIDSWITCH_STATE_CHANGE),
                    Some(GUID_SESSION_DISPLAY_STATUS),
                ] {
                    let mut handle = std::ptr::null_mut();
                    let result = unsafe {
                        match guid {
                            None => PowerRegisterSuspendResumeNotification(
                                DEVICE_NOTIFY_CALLBACK,
                                recipient,
                                &mut handle,
                            ),
                            Some(guid) => PowerSettingRegisterNotification(
                                &guid,
                                DEVICE_NOTIFY_CALLBACK,
                                recipient,
                                &mut handle,
                            ),
                        }
                    };
                    if result.0 == 0 {
                        registrations.push((HPOWERNOTIFY(handle as isize), guid.is_none()));
                    } else {
                        crate::log::cp_log(&format!(
                            "power registration {guid:?} failed: {}",
                            result.0
                        ));
                    }
                }
                // Registration's initial setting callback may be dispatched
                // asynchronously. Briefly gate first selection for its display
                // baseline; an absent sensor/API never stalls LogonUI.
                let _ = baseline_received.recv_timeout(std::time::Duration::from_millis(250));
                if let Some(runtime) = context.runtime.upgrade() {
                    runtime.finish_power_subscription(epoch);
                }
                let _ = receiver.recv();
                contexts()
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .remove(&token);
                let mut unregister_failed = false;
                for (handle, suspend) in registrations {
                    let result = unsafe {
                        if suspend {
                            PowerUnregisterSuspendResumeNotification(handle)
                        } else {
                            PowerSettingUnregisterNotification(handle)
                        }
                    };
                    if result.0 != 0 {
                        unregister_failed = true;
                        crate::log::cp_log(&format!("power unregister failed: {}", result.0));
                    }
                }
                // If unregister failed, retain the OS registration parameters.
                // Its revoked token is harmless and retains no runtime.
                if unregister_failed {
                    Box::leak(parameters);
                }
            });
        if spawned.is_err() {
            runtime.finish_power_subscription(epoch);
            crate::log::cp_log("power subscription thread spawn failed");
            return None;
        }
        Some(Self(stop))
    }
}
impl Drop for PowerEvents {
    fn drop(&mut self) {
        let _ = self.0.send(());
    }
}

unsafe extern "system" fn power_callback(
    context: *const c_void,
    kind: u32,
    setting: *const c_void,
) -> u32 {
    // No panics may unwind through this OS callback boundary.
    let _ = std::panic::catch_unwind(|| {
        let context = contexts()
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .get(&(context as usize))
            .cloned();
        let Some(context) = context else {
            return;
        };
        let event = match kind {
            PBT_APMSUSPEND => Some(PowerEvent::Suspend),
            PBT_APMRESUMEAUTOMATIC => Some(PowerEvent::ResumeAutomatic),
            PBT_APMRESUMESUSPEND => Some(PowerEvent::ResumeInteractive),
            PBT_POWERSETTINGCHANGE => unsafe { setting_event(setting) },
            _ => None,
        };
        if let (Some(event), Some(runtime)) = (event, context.runtime.upgrade()) {
            runtime.power_event(context.epoch, event);
            if matches!(event, PowerEvent::Display(_)) {
                let _ = context.display_baseline.send(());
            }
        }
    });
    0
}

unsafe fn setting_event(setting: *const c_void) -> Option<PowerEvent> {
    let setting = setting.cast::<POWERBROADCAST_SETTING>();
    if setting.is_null() {
        return None;
    }
    // Data is a variable-length tail, not the binding's one-byte array.
    let (guid, value) = unsafe {
        let guid = std::ptr::addr_of!((*setting).PowerSetting).read_unaligned();
        let length = std::ptr::addr_of!((*setting).DataLength).read_unaligned();
        if length != 4 {
            return None;
        }
        (
            guid,
            std::ptr::addr_of!((*setting).Data)
                .cast::<u32>()
                .read_unaligned(),
        )
    };
    if guid == GUID_LIDSWITCH_STATE_CHANGE && value <= 1 {
        Some(PowerEvent::Lid(value == 1))
    } else if guid == GUID_SESSION_DISPLAY_STATUS && value <= 2 {
        Some(PowerEvent::Display(value))
    } else {
        None
    }
}

pub(crate) fn locked_console_session(session: u32) -> bool {
    // Unit tests inject events, never query or change host power/session state.
    if cfg!(test) {
        return true;
    }
    unsafe {
        if session == u32::MAX || WTSGetActiveConsoleSessionId() != session {
            return false;
        }
        let mut buffer = PWSTR::null();
        let mut bytes = 0;
        if WTSQuerySessionInformationW(None, session, WTSSessionInfoEx, &mut buffer, &mut bytes)
            .is_err()
        {
            return false;
        }
        let allowed = if !buffer.is_null() && bytes as usize >= size_of::<WTSINFOEXW>() {
            let info = buffer.0.cast::<WTSINFOEXW>().read_unaligned();
            let state = info.Data.WTSInfoExLevel1;
            info.Level == 1
                && state.SessionId == session
                && state.SessionState == WTSActive
                && state.SessionFlags == WTS_SESSIONSTATE_LOCK as i32
        } else {
            false
        };
        WTSFreeMemory(buffer.0.cast());
        allowed
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[repr(C)]
    struct Setting {
        guid: windows_core::GUID,
        length: u32,
        value: u32,
    }
    #[test]
    fn revoked_callback_token_cannot_reach_runtime() {
        let runtime = AutoRuntime::new(
            PipeRecognitionTransport,
            Box::new(crate::auto_runtime::MonotonicClock),
        );
        let (baseline, _) = mpsc::channel();
        let token = NEXT_TOKEN
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
            .unwrap();
        contexts().lock().unwrap().insert(
            token,
            Arc::new(Context {
                runtime: Arc::downgrade(&runtime),
                epoch: 0,
                display_baseline: baseline,
            }),
        );
        let generation = runtime.generation();
        unsafe {
            power_callback(token as *const c_void, PBT_APMSUSPEND, std::ptr::null());
        }
        assert_ne!(runtime.generation(), generation);
        contexts().lock().unwrap().remove(&token);
        let generation = runtime.generation();
        unsafe {
            power_callback(token as *const c_void, PBT_APMSUSPEND, std::ptr::null());
        }
        assert_eq!(runtime.generation(), generation);
        runtime.shutdown();
    }

    #[test]
    fn decode_only_valid_known_payloads() {
        let mut value = Setting {
            guid: GUID_LIDSWITCH_STATE_CHANGE,
            length: 4,
            value: 0,
        };
        let decode = |v: &Setting| unsafe { setting_event((v as *const Setting).cast()) };
        assert_eq!(decode(&value), Some(PowerEvent::Lid(false)));
        value.value = 2;
        assert_eq!(decode(&value), None);
        value.guid = GUID_SESSION_DISPLAY_STATUS;
        assert_eq!(decode(&value), Some(PowerEvent::Display(2)));
        value.length = 3;
        assert_eq!(decode(&value), None);
        assert_eq!(unsafe { setting_event(std::ptr::null()) }, None);
    }
}
