//! Pure credential-tile copy and presentation policy. No Windows or locale I/O.
use crate::auto_recognition::{AttemptMachine, Outcome, Phase, StopReason};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Language {
    English,
    Chinese,
}

impl Language {
    pub fn from_lang_id(lang_id: u16) -> Self {
        if lang_id & 0x03ff == 0x04 {
            Self::Chinese
        } else {
            Self::English
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Message {
    Title,
    Tooltip,
    Password,
    Submit,
    InitialDelay,
    Recognizing,
    NoFace,
    Mismatch,
    RecoverableStop,
    Unavailable,
    LoginFailed,
    SavedPasswordRejected,
}

// (English, Simplified Chinese). Brand name stays the same in both languages.
const TEXT: [(&str, &str); 12] = [
    ("Smile2Unlock", "Smile2Unlock"),
    ("Face sign-in", "人脸登录"),
    ("Windows password", "Windows 密码"),
    ("Submit", "提交"),
    (
        "Face recognition will start shortly. You can enter your Windows password.",
        "即将开始人脸识别，也可以输入 Windows 密码。",
    ),
    (
        "Recognizing. Please face the camera, or enter your Windows password.",
        "正在识别，请正对摄像头。也可以输入 Windows 密码。",
    ),
    (
        "No face detected. Face the camera in good light.",
        "未检测到人脸，请正对摄像头并保持光线充足。",
    ),
    ("Face not recognized, retrying…", "人脸不匹配，正在重试…"),
    (
        "Face recognition stopped. Press Enter or click → to try again, or enter your Windows password.",
        "人脸识别已停止。按 Enter 或点击 → 重新识别，也可以输入 Windows 密码。",
    ),
    (
        "Face recognition is unavailable. Press Enter or click → to retry, or enter your Windows password.",
        "人脸识别暂不可用。按 Enter 或点击 → 重试，也可以输入 Windows 密码。",
    ),
    (
        "Face recognition unavailable. Enter your Windows password.",
        "人脸识别不可用，请输入 Windows 密码。",
    ),
    (
        "Update the saved Windows password in Smile2Unlock. Use your Windows password to sign in.",
        "请在 Smile2Unlock 中更新保存的 Windows 密码。本次请使用 Windows 密码登录。",
    ),
];

pub fn text(language: Language, message: Message) -> &'static str {
    let pair = TEXT[message as usize];
    match language {
        Language::English => pair.0,
        Language::Chinese => pair.1,
    }
}

pub fn tile_message(machine: &AttemptMachine, login_failed: bool) -> Message {
    if login_failed {
        return Message::LoginFailed;
    }
    match machine.phase() {
        Phase::InitialDelay => Message::InitialDelay,
        Phase::Recognizing | Phase::RetryDelay => match machine.retry_hint() {
            Some(Outcome::NoFace) => Message::NoFace,
            Some(Outcome::Mismatch) => Message::Mismatch,
            _ => Message::Recognizing,
        },
        Phase::Stopped => match machine.stop_reason() {
            Some(StopReason::Unavailable) => Message::Unavailable,
            _ => Message::RecoverableStop,
        },
        _ => Message::Title,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Poll, TriggerSettings};

    #[test]
    fn chinese_primary_language_uses_simplified_chinese() {
        for id in [0x0804, 0x0404, 0x0c04, 0x1004, 0x1404] {
            assert_eq!(Language::from_lang_id(id), Language::Chinese);
        }
        for id in [0, 0x0409, 0x0411, 0x0407] {
            assert_eq!(Language::from_lang_id(id), Language::English);
        }
        for (english, chinese) in TEXT {
            assert!(!english.is_empty() && !chinese.is_empty());
            assert!(english != chinese || english == "Smile2Unlock");
        }
        assert!(text(Language::Chinese, Message::RecoverableStop).contains("Enter"));
        assert!(!text(Language::English, Message::LoginFailed).contains("retry"));
    }

    #[test]
    fn unavailable_and_windows_failure_offer_different_actions() {
        let mut machine = AttemptMachine::new();
        let (generation, _) = machine.begin_on_demand(0, "test", 1, 1);
        machine.poll(0);
        machine.complete(generation, 1, Outcome::Fatal);
        assert_eq!(tile_message(&machine, false), Message::Unavailable);
        assert_eq!(tile_message(&machine, true), Message::LoginFailed);
        for language in [Language::Chinese, Language::English] {
            let unavailable = text(language, Message::Unavailable);
            assert!(unavailable.contains("Enter") && unavailable.contains('→'));
            let failed = text(language, Message::LoginFailed);
            assert!(!failed.contains("Press Enter") && !failed.contains("按 Enter"));
            assert!(!failed.contains('→'));
        }
    }

    #[test]
    fn retry_guidance_survives_refresh_and_next_capture() {
        let settings = TriggerSettings::from_values(Some(1), Some(0), Some(1), Some(30));
        for (outcome, message) in [
            (Outcome::NoFace, Message::NoFace),
            (Outcome::Mismatch, Message::Mismatch),
        ] {
            let mut machine = AttemptMachine::with_settings(settings);
            let (generation, _) = machine.begin(0, "test", 1, 1);
            machine.poll(0);
            assert_eq!(tile_message(&machine, false), Message::Recognizing);
            machine.complete(generation, 1, outcome);
            assert_eq!(tile_message(&machine, false), message);
            assert!(!machine.begin(500, "test", 1, 2).1);
            assert_eq!(tile_message(&machine, false), message);
            assert!(matches!(machine.poll(1001), Poll::Recognize(_)));
            assert_eq!(tile_message(&machine, false), message);
            machine.expire(30_000);
            assert_eq!(tile_message(&machine, false), Message::RecoverableStop);
            assert_eq!(tile_message(&machine, true), Message::LoginFailed);
            machine.begin_on_demand(31_000, "test", 1, 3);
            machine.poll(31_000);
            machine.complete(machine.generation(), 31_001, Outcome::Fatal);
            assert_eq!(tile_message(&machine, false), Message::Unavailable);
        }
    }
}
