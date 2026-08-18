//! Credential tile field layout for the Rust provider.
//!
//! This build tests the hypothesis that LogonUI needs the exact same field
//! count/order as the working C++ baseline. We therefore replicate the C++
//! baseline's full 14-field descriptor table from CredentialProvider/common.h,
//! keeping only the initial five fields visible (logo, hidden label, title,
//! focused password, submit button) and the remaining nine fields hidden.
//!
//! Numeric values below mirror the Windows SDK constants (ABI-stable; they
//! are passed into LogonUI as CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR).

/// CREDENTIAL_PROVIDER_FIELD_STATE
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldState {
    Hidden = 0,                  // CPFS_HIDDEN
    DisplayInSelectedTile = 1,   // CPFS_DISPLAY_IN_SELECTED_TILE
    DisplayInDeselectedTile = 2, // CPFS_DISPLAY_IN_DESELECTED_TILE
    DisplayInBoth = 3,           // CPFS_DISPLAY_IN_BOTH
}

/// CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE (official SDK, wincred.h/
/// credentialprovider.h — verified against winsdk-10 10.0.16299.0).
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InteractiveState {
    None = 0,     // CPFIS_NONE
    ReadOnly = 1, // CPFIS_READONLY
    Disabled = 2, // CPFIS_DISABLED
    Focused = 3,  // CPFIS_FOCUSED
}

/// CREDENTIAL_PROVIDER_FIELD_TYPE (official SDK values; the windows crate
/// 0.62.2 constants are correct — CPFT_TILE_IMAGE is 6, not 0. The earlier
/// "crate constants are wrong" note in the plan was itself wrong and caused
/// this local table to use the invalid 0..8 mapping; LogonUI rejects a
/// descriptor whose cpft is CPFT_INVALID (0)).
#[repr(i32)]
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldType {
    Invalid = 0,      // CPFT_INVALID
    LargeText = 1,    // CPFT_LARGE_TEXT
    SmallText = 2,    // CPFT_SMALL_TEXT
    CommandLink = 3,  // CPFT_COMMAND_LINK
    EditText = 4,     // CPFT_EDIT_TEXT
    PasswordText = 5, // CPFT_PASSWORD_TEXT
    TileImage = 6,    // CPFT_TILE_IMAGE
    Checkbox = 7,     // CPFT_CHECKBOX
    ComboBox = 8,     // CPFT_COMBOBOX
    SubmitButton = 9, // CPFT_SUBMIT_BUTTON
}

/// Field ids of the Rust v1 tile (visible set only; the nine hidden
/// baseline fields were removed in v15 — LogonUI only renders fields whose
/// state is DisplayInBoth/DisplayInSelectedTile, plus the focused input).
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldId {
    TileImage = 0,
    Label = 1,
    LargeText = 2,
    PasswordText = 3,
    SubmitButton = 4,
}

pub const FIELD_COUNT: usize = 5;

/// Per-field (state, interactive state).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FieldStatePair {
    pub state: FieldState,
    pub interactive: InteractiveState,
}

/// Matches the C++ baseline's visible field states (common.h
/// s_rgFieldStatePairs, hidden fields dropped).
pub fn state_pairs() -> [FieldStatePair; FIELD_COUNT] {
    [
        FieldStatePair {
            state: FieldState::DisplayInBoth,
            interactive: InteractiveState::None,
        }, // TileImage
        FieldStatePair {
            state: FieldState::Hidden,
            interactive: InteractiveState::None,
        }, // Label
        FieldStatePair {
            state: FieldState::DisplayInBoth,
            interactive: InteractiveState::None,
        }, // LargeText
        FieldStatePair {
            state: FieldState::DisplayInSelectedTile,
            interactive: InteractiveState::Focused,
        }, // PasswordText
        FieldStatePair {
            state: FieldState::DisplayInSelectedTile,
            interactive: InteractiveState::None,
        }, // SubmitButton
    ]
}

/// CREDENTIAL_PROVIDER_FIELD_TYPE for each field id (GetFieldDescriptorAt).
pub fn field_type(id: FieldId) -> FieldType {
    match id {
        FieldId::TileImage => FieldType::TileImage,
        FieldId::Label => FieldType::SmallText,
        FieldId::LargeText => FieldType::LargeText,
        FieldId::PasswordText => FieldType::PasswordText,
        FieldId::SubmitButton => FieldType::SubmitButton,
    }
}

/// CPFG_* field-type GUID data for a field that carries one.
pub fn field_type_guid(id: FieldId) -> (u32, u16, u16, [u8; 8]) {
    match id {
        // CPFG_CREDENTIAL_PROVIDER_LOGO
        FieldId::TileImage => (
            0x2d837775,
            0xf6cd,
            0x464e,
            [0xa7, 0x45, 0x48, 0x2f, 0xd0, 0xb4, 0x74, 0x93],
        ),
        // CPFG_CREDENTIAL_PROVIDER_LABEL
        FieldId::Label => (
            0x286bbff3,
            0xbad4,
            0x438f,
            [0xb0, 0x07, 0x79, 0xb7, 0x26, 0x7c, 0x3d, 0x48],
        ),
        _ => (0, 0, 0, [0; 8]),
    }
}

/// Field label for GetFieldDescriptorAt's pszLabel.
pub fn label(id: FieldId) -> &'static str {
    match id {
        FieldId::TileImage => "Smile2Unlock",
        FieldId::Label => "Tooltip",
        FieldId::LargeText => "Smile2Unlock Provider",
        FieldId::PasswordText => "Password text",
        FieldId::SubmitButton => "Submit",
    }
}

/// Display text for GetStringValue.
pub fn display_text(id: FieldId) -> &'static str {
    match id {
        FieldId::TileImage => "",
        FieldId::Label => "Smile2Unlock",
        FieldId::LargeText => "Smile2Unlock Provider",
        FieldId::PasswordText => "",
        FieldId::SubmitButton => "",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sdk_constants_match() {
        // Official wincred.h / credentialprovider.h values (winsdk-10
        // 10.0.16299.0). The windows crate 0.62.2 metadata is correct here.
        assert_eq!(FieldState::Hidden as i32, 0);
        assert_eq!(FieldState::DisplayInBoth as i32, 3);
        assert_eq!(InteractiveState::Focused as i32, 3);
        assert_eq!(InteractiveState::Disabled as i32, 2);
        assert_eq!(FieldType::Invalid as i32, 0);
        assert_eq!(FieldType::LargeText as i32, 1);
        assert_eq!(FieldType::SmallText as i32, 2);
        assert_eq!(FieldType::CommandLink as i32, 3);
        assert_eq!(FieldType::EditText as i32, 4);
        assert_eq!(FieldType::PasswordText as i32, 5);
        assert_eq!(FieldType::TileImage as i32, 6);
        assert_eq!(FieldType::Checkbox as i32, 7);
        assert_eq!(FieldType::ComboBox as i32, 8);
        assert_eq!(FieldType::SubmitButton as i32, 9);
    }

    #[test]
    fn field_count_matches_id_space() {
        assert_eq!(FIELD_COUNT, FieldId::SubmitButton as usize + 1);
    }

    #[test]
    fn layout_matches_cplusplus_baseline() {
        assert_eq!(field_type(FieldId::TileImage), FieldType::TileImage);
        assert_eq!(field_type(FieldId::Label), FieldType::SmallText);
        assert_eq!(field_type(FieldId::LargeText), FieldType::LargeText);
        assert_eq!(field_type(FieldId::PasswordText), FieldType::PasswordText);
        assert_eq!(field_type(FieldId::SubmitButton), FieldType::SubmitButton);

        let (d1, _, _, _) = field_type_guid(FieldId::TileImage);
        assert_eq!(d1, 0x2d837775); // CPFG_CREDENTIAL_PROVIDER_LOGO
        let (d1, _, _, _) = field_type_guid(FieldId::Label);
        assert_eq!(d1, 0x286bbff3); // CPFG_CREDENTIAL_PROVIDER_LABEL

        let pairs = state_pairs();
        assert_eq!(pairs[FieldId::Label as usize].state, FieldState::Hidden);
        assert_eq!(
            pairs[FieldId::PasswordText as usize].state,
            FieldState::DisplayInSelectedTile
        );
        assert_eq!(
            pairs[FieldId::PasswordText as usize].interactive,
            InteractiveState::Focused
        );
        assert_eq!(
            pairs[FieldId::LargeText as usize].state,
            FieldState::DisplayInBoth
        );
        assert_eq!(
            pairs[FieldId::SubmitButton as usize].state,
            FieldState::DisplayInSelectedTile
        );
    }
}
