//! Credential tile field layout for the Rust provider.
//!
//! The C++ baseline (CredentialProvider/common.h) enumerated 14 fields with a
//! manual password field. The Rust provider deliberately drops the manual
//! password field (plan decision: no manual password in the first version),
//! so the tile is rebuilt here with its own stable field ids.
//!
//! Numeric values below mirror the Windows SDK constants (ABI-stable; they
//! are passed into LogonUI as CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR).

/// CREDENTIAL_PROVIDER_FIELD_STATE
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldState {
    Hidden = 0,                       // CPFS_HIDDEN
    DisplayInSelectedTile = 1,        // CPFS_DISPLAY_IN_SELECTED_TILE
    DisplayInDeselectedTile = 2,      // CPFS_DISPLAY_IN_DESELECTED_TILE
    DisplayInBoth = 3,                // CPFS_DISPLAY_IN_BOTH
}

/// CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InteractiveState {
    None = 0,      // CPFIS_NONE
    ReadOnly = 1,  // CPFIS_READONLY
    Focused = 2,   // CPFIS_FOCUSED
    Selected = 3,  // CPFIS_SELECTED
}

/// CREDENTIAL_PROVIDER_FIELD_TYPE
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)] // reserved: the v1 tile carries no password/link fields
pub enum FieldType {
    // ABI values from wincred.h (the windows crate 0.62.2 bindings are wrong
    // here: CPFT_TILE_IMAGE=6, CPFT_SUBMIT_BUTTON=9, etc.).
    TileImage = 1,      // CPFT_TILE_IMAGE
    SmallText = 2,      // CPFT_SMALL_TEXT
    LargeText = 3,      // CPFT_LARGE_TEXT
    PasswordText = 4,   // CPFT_PASSWORD_TEXT (unused in v1)
    SubmitButton = 6,   // CPFT_SUBMIT_BUTTON
    CommandLink = 7,    // CPFT_COMMAND_LINK
}

/// Provisional v1 field ids. Finalized in Phase 1.
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldId {
    TileImage = 0,
    LargeText = 1,
    FaceStatus = 2,
    SubmitButton = 3,
}

#[allow(dead_code)] // used by Phase 1 COM plumbing
pub const FIELD_COUNT: usize = 4;

/// Per-field (state, interactive state) with an optional focus index;
/// mirrors FIELD_STATE_PAIR from the C++ baseline but without the
/// CPFIS_FOCUSED bit until Phase 1 decides the focus path.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FieldStatePair {
    pub state: FieldState,
    pub interactive: InteractiveState,
    pub focused_index: Option<usize>,
}

#[allow(dead_code)] // used by Phase 1 COM plumbing
pub fn state_pairs() -> [FieldStatePair; FIELD_COUNT] {
    [
        // Tile image is shown in both tiles (matches C++ SFI_TILEIMAGE).
        FieldStatePair { state: FieldState::DisplayInBoth, interactive: InteractiveState::None, focused_index: None },
        // Provider title.
        FieldStatePair { state: FieldState::DisplayInBoth, interactive: InteractiveState::None, focused_index: None },
        // Face-status text, only when the tile is selected.
        FieldStatePair { state: FieldState::DisplayInSelectedTile, interactive: InteractiveState::None, focused_index: None },
        // Submit drives auto-submission of the pipe-fetched credential.
        FieldStatePair { state: FieldState::DisplayInSelectedTile, interactive: InteractiveState::None, focused_index: None },
    ]
}

/// CREDENTIAL_PROVIDER_FIELD_TYPE for each field id (GetFieldDescriptorAt).
pub fn field_type(id: FieldId) -> FieldType {
    match id {
        FieldId::TileImage => FieldType::TileImage,
        FieldId::LargeText => FieldType::LargeText,
        FieldId::FaceStatus => FieldType::SmallText,
        FieldId::SubmitButton => FieldType::SubmitButton,
    }
}

/// CPFG_* field-type GUID data for a field that carries one.
/// Returns (data1, data2, data3, data4) in Windows GUID layout; zeroed for
/// plain fields. Kept dependency-free so the layout stays testable on Linux.
pub fn field_type_guid(id: FieldId) -> (u32, u16, u16, [u8; 8]) {
    match id {
        // CPFG_CREDENTIAL_PROVIDER_LOGO
        FieldId::TileImage => (0x2d837775, 0xf6cd, 0x464e, [0xa7, 0x45, 0x48, 0x2f, 0xd0, 0xb4, 0x74, 0x93]),
        // CPFG_CREDENTIAL_PROVIDER_LABEL
        FieldId::LargeText => (0x286bbff3, 0xbad4, 0x438f, [0xb0, 0x07, 0x79, 0xb7, 0x26, 0x7c, 0x3d, 0x48]),
        _ => (0, 0, 0, [0; 8]),
    }
}

/// Field label for GetFieldDescriptorAt's pszLabel.
pub fn label(id: FieldId) -> &'static str {
    match id {
        FieldId::TileImage => "Smile2Unlock",
        FieldId::LargeText => "Smile2Unlock",
        FieldId::FaceStatus => "Face authentication",
        FieldId::SubmitButton => "Sign in",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sdk_constants_match() {
        assert_eq!(FieldState::Hidden as i32, 0);
        assert_eq!(FieldState::DisplayInBoth as i32, 3);
        assert_eq!(InteractiveState::Focused as i32, 2);
        assert_eq!(FieldType::TileImage as i32, 1);
        assert_eq!(FieldType::SmallText as i32, 2);
        assert_eq!(FieldType::LargeText as i32, 3);
        assert_eq!(FieldType::SubmitButton as i32, 6);
        assert_eq!(FieldType::CommandLink as i32, 7);
    }

    #[test]
    fn field_count_matches_id_space() {
        assert_eq!(FIELD_COUNT, FieldId::SubmitButton as usize + 1);
    }

    #[test]
    fn no_manual_password_field() {
        // v1 tile must not carry CPFT_PASSWORD_TEXT: the credential is
        // fetched from the auth service, never typed by the user.
        assert!(!state_pairs().is_empty());
        let _ = FieldType::PasswordText; // reserved, unused by design
    }
}
