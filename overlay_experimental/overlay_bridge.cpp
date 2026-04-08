/*
 * GSE Overlay Bridge — C ABI implementation (lives inside steam_api DLL).
 *
 * This file is compiled ONLY in the api_experimental build (EMU_OVERLAY defined).
 * It exports functions that the ReShade addon calls via GetProcAddress.
 *
 * The bridge reads from Steam_Overlay / Settings / Steam_Overlay_Stats using
 * the public accessor methods added for this purpose.
 */

#ifdef EMU_OVERLAY

// winsock2.h MUST come before anything that might pull in winsock.h
// (dll.h → windows.h → winsock.h) to avoid type-redefinition errors.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "overlay_bridge.h"
#include "dll/dll.h"                    // get_steam_client()
#include "dll/dll/steam_client.h"       // Steam_Client → steam_overlay
#include "overlay/steam_overlay.h"

#include <cstring>
#include <algorithm>

// forward-declared in steam_overlay.cpp — we need the display info query
// We'll call the public method on Steam_Overlay instead.

/* ── helper: safe string copy ─────────────────────────────────────────── */
static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || !dst_sz) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static void safe_copy(char *dst, size_t dst_sz, const std::string &src)
{
    safe_copy(dst, dst_sz, src.c_str());
}

/* ── Exported bridge functions ────────────────────────────────────────── */

extern "C" {

__declspec(dllexport) uint32_t GSE_OverlayBridge_GetVersion(void)
{
    return GSE_BRIDGE_ABI_VERSION;
}

__declspec(dllexport) int GSE_OverlayBridge_GetState(GSE_OverlayState *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;

    auto *overlay = client->steam_overlay;
    auto *settings = client->settings_client;

    // Mark that the ReShade addon is connected — suppresses native overlay rendering
    overlay->Bridge_MarkConnected();

    out->abi_version = GSE_BRIDGE_ABI_VERSION;
    out->app_id = settings ? settings->get_local_game_id().AppID() : 0;
    out->is_ready = overlay->Ready() ? 1 : 0;
    out->show_overlay = overlay->ShowOverlay() ? 1 : 0;
    out->warn_local_save = overlay->Bridge_GetWarnLocalSave() ? 1 : 0;
    out->warn_bad_appid = overlay->Bridge_GetWarnBadAppId() ? 1 : 0;

    // Renderer API — the addon is rendering through ReShade, so report unknown
    out->renderer_api = GSE_RENDERER_UNKNOWN;

    // Notification position
    out->notif_position = static_cast<uint8_t>(overlay->Bridge_GetNotifPosition());

    // Stats
    auto stats_state = overlay->Bridge_GetStatsState();
    out->show_fps = stats_state.show_fps ? 1 : 0;
    out->show_frametime = stats_state.show_frametime ? 1 : 0;
    out->show_playtime = stats_state.show_playtime ? 1 : 0;
    out->active_fps = stats_state.fps;
    out->active_frametime_ms = stats_state.frametime_ms;
    out->active_playtime_hr = stats_state.playtime_hr;
    out->active_playtime_min = stats_state.playtime_min;
    out->active_playtime_sec = stats_state.playtime_sec;

    // Strings
    if (settings) {
        safe_copy(out->username, sizeof(out->username), settings->get_local_name());
        safe_copy(out->language, sizeof(out->language), settings->get_language());
    }

#ifndef EMU_BUILD_STRING
    #define EMU_BUILD_STRING "unknown"
#endif
#ifndef EMU_BUILD_DATE_STRING
    #define EMU_BUILD_DATE_STRING "unknown"
#endif
    safe_copy(out->build_string, sizeof(out->build_string), EMU_BUILD_STRING);
    safe_copy(out->build_date, sizeof(out->build_date), EMU_BUILD_DATE_STRING);

    return 1;
}

__declspec(dllexport) void GSE_OverlayBridge_ShowOverlay(int show)
{
    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return;
    client->steam_overlay->ShowOverlay(show != 0);
}

__declspec(dllexport) int GSE_OverlayBridge_GetAchievementCount(void)
{
    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;
    return client->steam_overlay->Bridge_GetAchievementCount();
}

__declspec(dllexport) int GSE_OverlayBridge_GetAchievements(GSE_Achievement *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;

    return client->steam_overlay->Bridge_GetAchievements(out, max_count);
}

__declspec(dllexport) int GSE_OverlayBridge_GetNotifications(GSE_Notification *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;

    return client->steam_overlay->Bridge_GetNotifications(out, max_count);
}

__declspec(dllexport) void GSE_OverlayBridge_ExpireNotification(int id)
{
    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return;
    client->steam_overlay->Bridge_ExpireNotification(id);
}

__declspec(dllexport) int GSE_OverlayBridge_GetDisplayInfo(GSE_DisplayInfo *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;

    return client->steam_overlay->Bridge_GetDisplayInfo(out, max_count);
}

__declspec(dllexport) float GSE_OverlayBridge_GetSDRWhiteScale(void)
{
    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 1.0f;
    return client->steam_overlay->Bridge_GetSDRWhiteScale();
}

__declspec(dllexport) int GSE_OverlayBridge_GetFriendCount(void)
{
    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;
    return client->steam_overlay->Bridge_GetFriendCount();
}

__declspec(dllexport) int GSE_OverlayBridge_GetFriends(GSE_Friend *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    auto *client = get_steam_client();
    if (!client || !client->steam_overlay) return 0;

    return client->steam_overlay->Bridge_GetFriends(out, max_count);
}

__declspec(dllexport) int GSE_OverlayBridge_GetOption(int option_id)
{
    auto *client = get_steam_client();
    if (!client) return 0;

    auto *s = client->settings_client;
    auto *overlay = client->steam_overlay;
    if (!s) return 0;

    switch (option_id) {
    case GSE_OPT_FRIEND_NOTIF_ENABLE:          return !s->disable_overlay_friend_notification;
    case GSE_OPT_ACH_NOTIF_ENABLE:             return !s->disable_overlay_achievement_notification;
    case GSE_OPT_INVITE_NOTIF_ENABLE:          return 1; // always enabled for now
    case GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE:    return !s->disable_overlay_achievement_progress;
    case GSE_OPT_SORT_BY_GLOBAL_PERCENT:       return s->overlay_achievement_sort_by_global_percent;
    case GSE_OPT_LOCAL_SAVE_WARNING:           return s->overlay_warn_local_save;
    case GSE_OPT_ALWAYS_SHOW_USER:             return s->overlay_always_show_user_info;
    case GSE_OPT_ALWAYS_SHOW_STATS:            return s->overlay_always_show_fps || s->overlay_always_show_frametime || s->overlay_always_show_playtime;
    case GSE_OPT_DISABLE_ALL_WARNINGS:         return s->disable_overlay_warning_any;
    case GSE_OPT_DISABLE_BAD_APPID_WARNING:    return s->disable_overlay_warning_bad_appid;
    case GSE_OPT_DISABLE_LOCAL_SAVE_WARNING:   return s->disable_overlay_warning_local_save;
    case GSE_OPT_NOTIF_POSITION:               return overlay ? overlay->Bridge_GetNotifPosition() : 0;
    default: return 0;
    }
}

__declspec(dllexport) void GSE_OverlayBridge_SetOption(int option_id, int value)
{
    auto *client = get_steam_client();
    if (!client) return;

    auto *s = client->settings_client;
    if (!s) return;

    switch (option_id) {
    case GSE_OPT_FRIEND_NOTIF_ENABLE:          s->disable_overlay_friend_notification = !value; break;
    case GSE_OPT_ACH_NOTIF_ENABLE:             s->disable_overlay_achievement_notification = !value; break;
    case GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE:    s->disable_overlay_achievement_progress = !value; break;
    case GSE_OPT_SORT_BY_GLOBAL_PERCENT:       s->overlay_achievement_sort_by_global_percent = !!value; break;
    case GSE_OPT_LOCAL_SAVE_WARNING:           s->overlay_warn_local_save = !!value; break;
    case GSE_OPT_ALWAYS_SHOW_USER:             s->overlay_always_show_user_info = !!value; break;
    case GSE_OPT_DISABLE_ALL_WARNINGS:         s->disable_overlay_warning_any = !!value; break;
    case GSE_OPT_DISABLE_BAD_APPID_WARNING:    s->disable_overlay_warning_bad_appid = !!value; break;
    case GSE_OPT_DISABLE_LOCAL_SAVE_WARNING:   s->disable_overlay_warning_local_save = !!value; break;
    default: break;
    }

    // Trigger settings save
    if (client->steam_overlay) {
        client->steam_overlay->Bridge_RequestSaveSettings();
    }
}

} // extern "C"

#endif // EMU_OVERLAY
