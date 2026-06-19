//
// HomeFaceLogon - Credential Provider Tile Field Definitions
//
// Defines the fields displayed on the logon/unlock tile:
//   - Tile image (user avatar)
//   - Title text ("Home Face Logon")
//   - PIN input field (fallback when face auth fails)
//   - Submit button
//   - Retry face scan link
//   - Status text (camera/matching progress)
//

#pragma once
#include "helpers.h"

// Field IDs for our credential provider tile.
// Stripped to only the fields actually used by HomeFaceLogon.
enum SAMPLE_FIELD_ID
{
    SFI_TILEIMAGE         = 0,
    SFI_LARGE_TEXT        = 1,
    SFI_PASSWORD          = 2,  // Used as PIN input field
    SFI_SUBMIT_BUTTON     = 3,
    SFI_LAUNCHWINDOW_LINK = 4,  // "Retry face scan" link
    SFI_LOGONSTATUS_TEXT  = 5,
    SFI_NUM_FIELDS        = 6,
};

// The first value indicates when the tile is displayed (selected, not selected)
// the second indicates things like whether the field is enabled, whether it has key focus, etc.
struct FIELD_STATE_PAIR
{
    CREDENTIAL_PROVIDER_FIELD_STATE cpfs;
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE cpfis;
};

// Field state: controls visibility in selected vs deselected tile states.
static const FIELD_STATE_PAIR s_rgFieldStatePairs[] =
{
    { CPFS_DISPLAY_IN_BOTH,            CPFIS_NONE    },    // SFI_TILEIMAGE
    { CPFS_DISPLAY_IN_BOTH,            CPFIS_NONE    },    // SFI_LARGE_TEXT
    { CPFS_DISPLAY_IN_SELECTED_TILE,   CPFIS_FOCUSED },    // SFI_PASSWORD (PIN input)
    { CPFS_DISPLAY_IN_SELECTED_TILE,   CPFIS_NONE    },    // SFI_SUBMIT_BUTTON
    { CPFS_DISPLAY_IN_SELECTED_TILE,   CPFIS_NONE    },    // SFI_LAUNCHWINDOW_LINK
    { CPFS_DISPLAY_IN_SELECTED_TILE,   CPFIS_NONE    },    // SFI_LOGONSTATUS_TEXT
};

// Field descriptors for unlock and logon.
static const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR s_rgCredProvFieldDescriptors[] =
{
    { SFI_TILEIMAGE,         CPFT_TILE_IMAGE,    L"Image",                      CPFG_CREDENTIAL_PROVIDER_LOGO  },
    { SFI_LARGE_TEXT,        CPFT_LARGE_TEXT,     L"Home Face Logon"                                            },
    { SFI_PASSWORD,          CPFT_PASSWORD_TEXT,  L"PIN \x3092\x5165\x529B"                                     },  // "PINを入力"
    { SFI_SUBMIT_BUTTON,     CPFT_SUBMIT_BUTTON,  L"\x30B5\x30A4\x30F3\x30A4\x30F3"                            },  // "サインイン"
    { SFI_LAUNCHWINDOW_LINK, CPFT_COMMAND_LINK,   L"\x9854\x7167\x5408\x3092\x518D\x8A66\x884C"                },  // "顔照合を再試行"
    { SFI_LOGONSTATUS_TEXT,  CPFT_SMALL_TEXT,     L"\x30AB\x30E1\x30E9\x306E\x6E96\x5099\x4E2D..."             },  // "カメラの準備中..."
};
