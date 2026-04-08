/*
 * GSE Overlay Bridge — C ABI shared between steam_api DLL and ReShade addon.
 * Version 7 (clean rewrite).
 *
 * The emu DLL (steam_api.dll / steam_api64.dll) exports functions prefixed
 * with GSE_OverlayBridge_.  The ReShade addon resolves them at load time via
 * GetProcAddress and calls them each frame from the render thread.
 *
 * ALL DATA RETURNED BY BRIDGE FUNCTIONS IS ONLY VALID UNTIL THE NEXT CALL
 * TO THE SAME FUNCTION.  The addon must copy anything it needs to keep.
 */

#ifndef GSE_OVERLAY_BRIDGE_H
#define GSE_OVERLAY_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI version ──────────────────────────────────────────────────────── */

#define GSE_BRIDGE_ABI_VERSION 7

/* ── Enums ────────────────────────────────────────────────────────────── */

enum GSE_NotifPosition {
    GSE_NOTIF_POS_TOP_LEFT     = 0,
    GSE_NOTIF_POS_TOP_RIGHT    = 1,
    GSE_NOTIF_POS_BOT_LEFT     = 2,
    GSE_NOTIF_POS_BOT_RIGHT    = 3,
    GSE_NOTIF_POS_TOP_CENTER   = 4,
    GSE_NOTIF_POS_BOT_CENTER   = 5,
};

enum GSE_NotifType {
    GSE_NOTIF_MESSAGE            = 0,
    GSE_NOTIF_INVITE             = 1,
    GSE_NOTIF_ACHIEVEMENT        = 2,
    GSE_NOTIF_ACHIEVEMENT_PROG   = 3,
    GSE_NOTIF_AUTO_ACCEPT_INVITE = 4,
};

enum GSE_RendererAPI {
    GSE_RENDERER_UNKNOWN = 0,
    GSE_RENDERER_DX9     = 1,
    GSE_RENDERER_DX10    = 2,
    GSE_RENDERER_DX11    = 3,
    GSE_RENDERER_DX12    = 4,
    GSE_RENDERER_OPENGL  = 5,
    GSE_RENDERER_VULKAN  = 6,
};

/* ── Data structs (C-compatible, layout-stable) ───────────────────────── */

typedef struct GSE_OverlayState {
    uint32_t abi_version;           /* must be GSE_BRIDGE_ABI_VERSION */
    uint32_t app_id;
    uint8_t  is_ready;              /* overlay infrastructure ready? */
    uint8_t  show_overlay;          /* main overlay visible? */
    uint8_t  warn_local_save;
    uint8_t  warn_bad_appid;
    uint8_t  renderer_api;          /* GSE_RendererAPI */
    uint8_t  notif_position;        /* GSE_NotifPosition */
    uint8_t  show_fps;
    uint8_t  show_frametime;
    uint8_t  show_playtime;
    uint8_t  _pad[3];
    float    active_fps;
    float    active_frametime_ms;
    float    active_playtime_hr;
    float    active_playtime_min;
    float    active_playtime_sec;
    char     username[256];
    char     language[64];
    char     build_string[128];
    char     build_date[64];
} GSE_OverlayState;

typedef struct GSE_Achievement {
    char     name[256];             /* internal schema name */
    char     title[256];            /* display name */
    char     description[512];
    uint32_t progress;
    uint32_t max_progress;
    uint32_t unlock_time;
    uint8_t  hidden;
    uint8_t  achieved;
    uint8_t  _pad[2];
    float    global_percent;        /*  0..100, -1 if unknown */
    /* Icon pixel data pointers.
     * These point into the emu's decoded icon buffers.
     * Valid only until the next call to GSE_OverlayBridge_GetAchievements.
     * If icon_pixels == NULL, the icon hasn't been decoded yet. */
    const uint8_t *icon_pixels;     /* RGBA, icon_w * icon_h * 4 bytes */
    int32_t  icon_w;
    int32_t  icon_h;
    const uint8_t *icon_gray_pixels;
    int32_t  icon_gray_w;
    int32_t  icon_gray_h;
} GSE_Achievement;

typedef struct GSE_Notification {
    int32_t  id;
    uint8_t  type;                  /* GSE_NotifType */
    uint8_t  _pad[3];
    int64_t  start_time_ms;         /* steady_clock ms since epoch */
    int64_t  duration_ms;
    char     message[512];
    /* For achievement notifications: */
    char     ach_title[256];
    uint32_t ach_progress;
    uint32_t ach_max_progress;
    uint8_t  ach_achieved;
    uint8_t  _pad2[3];
    /* Achievement icon (same semantics as GSE_Achievement) */
    const uint8_t *ach_icon_pixels;
    int32_t  ach_icon_w;
    int32_t  ach_icon_h;
} GSE_Notification;

typedef struct GSE_DisplayInfo {
    char     name[128];
    char     encoding[16];
    char     gamut[24];
    char     transfer[16];
    char     range[8];
    uint8_t  hdr_supported;
    uint8_t  hdr_enabled;
    uint8_t  wide_color;
    uint8_t  force_disabled;
    int32_t  bpc;
    int32_t  sdr_white_nits;
} GSE_DisplayInfo;

typedef struct GSE_Friend {
    uint64_t steam_id;
    char     name[128];
    uint8_t  is_online;
    uint8_t  is_joinable;
    uint8_t  window_state;          /* bitmask: show, invite, join, etc. */
    uint8_t  _pad[5];
} GSE_Friend;

/* ── Function pointer typedefs (for GetProcAddress) ───────────────────── */

/* Core */
typedef uint32_t  (*pfn_GSE_OverlayBridge_GetVersion)(void);
typedef int       (*pfn_GSE_OverlayBridge_GetState)(GSE_OverlayState *out);
typedef void      (*pfn_GSE_OverlayBridge_ShowOverlay)(int show);

/* Achievements */
typedef int       (*pfn_GSE_OverlayBridge_GetAchievementCount)(void);
typedef int       (*pfn_GSE_OverlayBridge_GetAchievements)(GSE_Achievement *out, int max_count);

/* Notifications — returns count of active (non-expired) notifications */
typedef int       (*pfn_GSE_OverlayBridge_GetNotifications)(GSE_Notification *out, int max_count);
typedef void      (*pfn_GSE_OverlayBridge_ExpireNotification)(int id);

/* Display / HDR info */
typedef int       (*pfn_GSE_OverlayBridge_GetDisplayInfo)(GSE_DisplayInfo *out, int max_count);
typedef float     (*pfn_GSE_OverlayBridge_GetSDRWhiteScale)(void);

/* Friends */
typedef int       (*pfn_GSE_OverlayBridge_GetFriendCount)(void);
typedef int       (*pfn_GSE_OverlayBridge_GetFriends)(GSE_Friend *out, int max_count);

/* Settings read/write (key-value string pairs) */
typedef int       (*pfn_GSE_OverlayBridge_GetOption)(int option_id);
typedef void      (*pfn_GSE_OverlayBridge_SetOption)(int option_id, int value);

/* Option IDs for Get/SetOption */
#define GSE_OPT_FRIEND_NOTIF_ENABLE          1  /* bool */
#define GSE_OPT_ACH_NOTIF_ENABLE             2  /* bool */
#define GSE_OPT_INVITE_NOTIF_ENABLE          3  /* bool */
#define GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE    4  /* bool */
#define GSE_OPT_SORT_BY_GLOBAL_PERCENT       5  /* bool */
#define GSE_OPT_LOCAL_SAVE_WARNING           6  /* bool */
#define GSE_OPT_ALWAYS_SHOW_USER             7  /* bool */
#define GSE_OPT_ALWAYS_SHOW_STATS            8  /* bool */
#define GSE_OPT_DISABLE_ALL_WARNINGS         9  /* bool */
#define GSE_OPT_DISABLE_BAD_APPID_WARNING   10  /* bool */
#define GSE_OPT_DISABLE_LOCAL_SAVE_WARNING  11  /* bool */
#define GSE_OPT_NOTIF_POSITION              12  /* GSE_NotifPosition */

/* ── Convenience: load all bridge functions from a module ─────────────── */

typedef struct GSE_BridgeFunctions {
    pfn_GSE_OverlayBridge_GetVersion          GetVersion;
    pfn_GSE_OverlayBridge_GetState            GetState;
    pfn_GSE_OverlayBridge_ShowOverlay         ShowOverlay;
    pfn_GSE_OverlayBridge_GetAchievementCount GetAchievementCount;
    pfn_GSE_OverlayBridge_GetAchievements     GetAchievements;
    pfn_GSE_OverlayBridge_GetNotifications    GetNotifications;
    pfn_GSE_OverlayBridge_ExpireNotification  ExpireNotification;
    pfn_GSE_OverlayBridge_GetDisplayInfo      GetDisplayInfo;
    pfn_GSE_OverlayBridge_GetSDRWhiteScale    GetSDRWhiteScale;
    pfn_GSE_OverlayBridge_GetFriendCount      GetFriendCount;
    pfn_GSE_OverlayBridge_GetFriends          GetFriends;
    pfn_GSE_OverlayBridge_GetOption           GetOption;
    pfn_GSE_OverlayBridge_SetOption           SetOption;
} GSE_BridgeFunctions;

#ifdef _WIN32
#include <Windows.h>
static inline int GSE_LoadBridgeFunctions(HMODULE emu_dll, GSE_BridgeFunctions *fn)
{
    if (!emu_dll || !fn) return 0;
    #define LOAD(name) fn->name = (pfn_GSE_OverlayBridge_##name)GetProcAddress(emu_dll, "GSE_OverlayBridge_" #name)
    LOAD(GetVersion);
    LOAD(GetState);
    LOAD(ShowOverlay);
    LOAD(GetAchievementCount);
    LOAD(GetAchievements);
    LOAD(GetNotifications);
    LOAD(ExpireNotification);
    LOAD(GetDisplayInfo);
    LOAD(GetSDRWhiteScale);
    LOAD(GetFriendCount);
    LOAD(GetFriends);
    LOAD(GetOption);
    LOAD(SetOption);
    #undef LOAD
    /* At minimum, GetVersion must be present */
    return fn->GetVersion != NULL;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* GSE_OVERLAY_BRIDGE_H */
