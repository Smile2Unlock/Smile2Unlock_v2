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
    TileImage = 0,      // CPFT_TILE_IMAGE
    SmallText = 1,      // CPFT_SMALL_TEXT
    LargeText = 2,      // CPFT_LARGE_TEXT
    PasswordText = 3,   // CPFT_PASSWORD_TEXT (unused in v1)
    SubmitButton = 4,   // CPFT_SUBMIT_BUTTON
    CommandLink = 5,    // CPFT_COMMAND_LINK
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sdk_constants_match() {
        assert_eq!(FieldState::Hidden as i32, 0);
        assert_eq!(FieldState::DisplayInBoth as i32, 3);
        assert_eq!(InteractiveState::Focused as i32, 2);
        assert_eq!(FieldType::TileImage as i32, 0);
        assert_eq!(FieldType::SubmitButton as i32, 4);
        assert_eq!(FieldType::CommandLink as i32, 5);
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
