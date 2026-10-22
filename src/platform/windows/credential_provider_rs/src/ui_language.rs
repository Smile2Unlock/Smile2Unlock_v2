//! Read the Windows UI language, independently of region/number formatting.
use crate::ui_text::{Language, Message};

pub fn text(message: Message) -> &'static str {
    #[cfg(windows)]
    let language = Language::from_lang_id(unsafe {
        windows::Win32::Globalization::GetUserDefaultUILanguage()
    });
    #[cfg(not(windows))]
    let language = Language::from_lang_id(0x0409);
    crate::ui_text::text(language, message)
}
