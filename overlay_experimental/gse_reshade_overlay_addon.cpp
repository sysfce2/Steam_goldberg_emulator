/*
 * GSE Overlay — ReShade Addon
 *
 * This addon replaces ingame_overlay's rendering when the user has ReShade
 * installed.  It communicates with the emu DLL (steam_api.dll) through the
 * GSE_OverlayBridge C ABI, and renders through ReShade's ImGui integration.
 *
 * Build as a separate DLL with .addon64 / .addon extension.
 * Place in the game directory alongside ReShade's dxgi.dll.
 *
 * Architecture:
 *   steam_api.dll  ──(exports GSE_OverlayBridge_*)──>  gse_overlay.addon
 *        │                                                    │
 *        │  achievement/friend/notification data              │  ImGui rendering
 *        │  flows through C ABI structs                       │  via ReShade overlay
 *        └────────────────────────────────────────────────────┘
 */

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "overlay_bridge.h"

#include <cstring>
#include <cstdio>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

using namespace reshade::api;

/* ── Addon metadata (required exports) ────────────────────────────────── */

extern "C" __declspec(dllexport) const char *NAME        = "GSE Overlay";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Steam emu overlay rendered through ReShade";

/* ── Bridge connection state ──────────────────────────────────────────── */

static HMODULE          s_emu_dll  = nullptr;
static GSE_BridgeFunctions s_bridge  = {};
static bool             s_bridge_ok = false;

/* ── Per-device GPU texture cache (achievement icons) ─────────────────── */

struct IconTexture {
    resource     tex  = {};
    resource_view srv = {};
    int          w    = 0;
    int          h    = 0;
    bool         valid = false;
};

struct __declspec(uuid("a1b2c3d4-1234-5678-abcd-ef0123456789")) addon_device_data
{
    std::unordered_map<std::string, IconTexture> icon_cache; // key = ach name + "_achieved"/"_locked"
};

/* ── Forward declarations ──────────────────────────────────────────────── */

static void render_main_overlay(effect_runtime *runtime);
static void render_achievement_list(effect_runtime *runtime);

/* ── Overlay toggle state ─────────────────────────────────────────────── */

static bool s_show_main_overlay = false;
static bool s_show_achievements = false;

/* ── FPS tracking (addon-side) ────────────────────────────────────────── */

static float s_addon_fps = 0.0f;
static float s_addon_frametime = 0.0f;
static int   s_frame_count = 0;
static std::chrono::steady_clock::time_point s_last_fps_time = std::chrono::steady_clock::now();

static void update_fps()
{
    auto now = std::chrono::steady_clock::now();
    s_frame_count++;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_fps_time).count();
    if (elapsed >= 500) {
        s_addon_fps = s_frame_count * 1000.0f / elapsed;
        s_addon_frametime = elapsed / (float)s_frame_count;
        s_frame_count = 0;
        s_last_fps_time = now;
    }
}

/* ── Playtime tracking (addon-side) ───────────────────────────────────── */

static std::chrono::steady_clock::time_point s_start_time = std::chrono::steady_clock::now();

static void get_playtime(float &hr, float &min, float &sec)
{
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_start_time).count();
    hr  = (float)(elapsed / 3600);
    min = (float)((elapsed % 3600) / 60);
    sec = (float)(elapsed % 60);
}

/* ── Try to find and connect to the emu DLL ───────────────────────────── */

static bool try_connect_bridge()
{
    if (s_bridge_ok) return true;

    // Try common emu DLL names
    const char *names[] = { "steam_api64.dll", "steam_api.dll" };
    for (auto name : names) {
        s_emu_dll = GetModuleHandleA(name);
        if (s_emu_dll) break;
    }

    if (!s_emu_dll) return false;

    if (!GSE_LoadBridgeFunctions(s_emu_dll, &s_bridge)) return false;

    uint32_t ver = s_bridge.GetVersion();
    if (ver < GSE_BRIDGE_ABI_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "GSE bridge ABI version mismatch (emu=%u, addon=%u)", ver, GSE_BRIDGE_ABI_VERSION);
        reshade::log::message(reshade::log::level::warning, msg);
    }

    s_bridge_ok = true;
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "GSE bridge connected (ABI v%u)", ver);
        reshade::log::message(reshade::log::level::info, msg);
    }
    return true;
}

/* ── Helper: upload RGBA pixels to a GPU texture ──────────────────────── */

static IconTexture upload_icon(device *dev, const uint8_t *pixels, int w, int h)
{
    IconTexture icon{};
    if (!dev || !pixels || w <= 0 || h <= 0) return icon;

    subresource_data init{};
    init.data       = const_cast<uint8_t*>(pixels);
    init.row_pitch  = w * 4;
    init.slice_pitch = w * h * 4;

    resource_desc desc(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        1, 1, format::r8g8b8a8_unorm, 1, memory_heap::default_,
        resource_usage::shader_resource);

    if (!dev->create_resource(desc, &init, resource_usage::shader_resource, &icon.tex)) {
        return icon;
    }

    if (!dev->create_resource_view(icon.tex, resource_usage::shader_resource,
            resource_view_desc(format::r8g8b8a8_unorm), &icon.srv)) {
        dev->destroy_resource(icon.tex);
        icon.tex = {};
        return icon;
    }

    icon.w = w;
    icon.h = h;
    icon.valid = true;
    return icon;
}

static void free_icon(device *dev, IconTexture &icon)
{
    if (!dev) return;
    if (icon.srv.handle) dev->destroy_resource_view(icon.srv);
    if (icon.tex.handle) dev->destroy_resource(icon.tex);
    icon = {};
}

/* ── Device lifecycle ─────────────────────────────────────────────────── */

static void on_init_device(device *dev)
{
    dev->create_private_data<addon_device_data>();
}

static void on_destroy_device(device *dev)
{
    auto *data = dev->get_private_data<addon_device_data>();
    if (data) {
        for (auto &[key, icon] : data->icon_cache)
            free_icon(dev, icon);
        dev->destroy_private_data<addon_device_data>();
    }
}

/* ── Notification rendering (always visible, via reshade_overlay event) ─ */

static void render_notifications(effect_runtime *runtime)
{
    if (!s_bridge_ok || !s_bridge.GetNotifications) return;

    GSE_Notification notifs[16];
    int count = s_bridge.GetNotifications(notifs, 16);
    if (count <= 0) return;

    auto &io = ImGui::GetIO();
    float screen_w = io.DisplaySize.x;
    float screen_h = io.DisplaySize.y;

    // Get notification position preference
    int notif_pos = GSE_NOTIF_POS_BOT_LEFT;
    if (s_bridge.GetOption)
        notif_pos = s_bridge.GetOption(GSE_OPT_NOTIF_POSITION);

    float margin = 10.0f;
    float notif_w = screen_w * 0.25f;
    if (notif_w < 300.0f) notif_w = 300.0f;
    float y_offset = margin;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    for (int i = 0; i < count; ++i) {
        auto &n = notifs[i];

        int64_t elapsed = now_ms - n.start_time_ms;
        if (elapsed < 0) elapsed = 0;
        if (n.duration_ms > 0 && elapsed > n.duration_ms) {
            // Expire this notification
            if (s_bridge.ExpireNotification)
                s_bridge.ExpireNotification(n.id);
            continue;
        }

        // Animation factor (slide in/out)
        float alpha = 1.0f;
        float slide = 0.0f;
        int64_t anim_ms = 350;
        if (elapsed < anim_ms) {
            float t = (float)elapsed / (float)anim_ms;
            alpha = t;
            slide = (1.0f - t) * notif_w;
        } else if (n.duration_ms > 0 && elapsed > n.duration_ms - anim_ms) {
            float t = (float)(n.duration_ms - elapsed) / (float)anim_ms;
            alpha = t;
            slide = (1.0f - t) * notif_w;
        }

        // Position
        float x, y;
        switch (notif_pos) {
        default:
        case GSE_NOTIF_POS_BOT_LEFT:
            x = margin - slide;
            y = screen_h - margin - y_offset - 80.0f;
            break;
        case GSE_NOTIF_POS_BOT_RIGHT:
            x = screen_w - notif_w - margin + slide;
            y = screen_h - margin - y_offset - 80.0f;
            break;
        case GSE_NOTIF_POS_TOP_LEFT:
            x = margin - slide;
            y = margin + y_offset;
            break;
        case GSE_NOTIF_POS_TOP_RIGHT:
            x = screen_w - notif_w - margin + slide;
            y = margin + y_offset;
            break;
        case GSE_NOTIF_POS_TOP_CENTER:
            x = (screen_w - notif_w) * 0.5f;
            y = margin + y_offset;
            break;
        case GSE_NOTIF_POS_BOT_CENTER:
            x = (screen_w - notif_w) * 0.5f;
            y = screen_h - margin - y_offset - 80.0f;
            break;
        }

        ImGui::SetNextWindowPos(ImVec2(x, y));
        ImGui::SetNextWindowSize(ImVec2(notif_w, 0));
        ImGui::SetNextWindowBgAlpha(0.85f * alpha);

        char win_id[64];
        snprintf(win_id, sizeof(win_id), "##gse_notif_%d", n.id);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        if (ImGui::Begin(win_id, nullptr, flags)) {
            // Notification content
            switch (n.type) {
            case GSE_NOTIF_ACHIEVEMENT:
            case GSE_NOTIF_ACHIEVEMENT_PROG: {
                bool is_unlock = (n.type == GSE_NOTIF_ACHIEVEMENT) && n.ach_achieved;
                if (is_unlock)
                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "Achievement Unlocked!");
                else
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Achievement Progress");

                if (n.ach_title[0])
                    ImGui::Text("%s", n.ach_title);

                if (n.ach_max_progress > 0 && !is_unlock) {
                    float frac = (float)n.ach_progress / (float)n.ach_max_progress;
                    ImGui::ProgressBar(frac, ImVec2(-1, 0));
                }
                break;
            }
            case GSE_NOTIF_INVITE:
                ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "Game Invite");
                ImGui::Text("%s", n.message);
                break;
            case GSE_NOTIF_AUTO_ACCEPT_INVITE:
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Auto-Accepted Invite");
                ImGui::Text("%s", n.message);
                break;
            default:
                ImGui::Text("%s", n.message);
                break;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();

        y_offset += 90.0f; // stack notifications vertically
    }
}

/* ── Stats HUD (always visible when enabled) ──────────────────────────── */

static void render_stats_hud()
{
    if (!s_bridge_ok || !s_bridge.GetState) return;

    GSE_OverlayState state{};
    s_bridge.GetState(&state);

    bool any_stats = state.show_fps || state.show_frametime || state.show_playtime;
    if (!any_stats) return;

    ImGui::SetNextWindowPos(ImVec2(5, 5));
    ImGui::SetNextWindowBgAlpha(0.4f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##gse_stats", nullptr, flags)) {
        if (state.show_fps)
            ImGui::Text("FPS: %.0f", s_addon_fps);
        if (state.show_frametime)
            ImGui::Text("Frame: %.1f ms", s_addon_frametime);
        if (state.show_playtime) {
            float hr, mn, sc;
            get_playtime(hr, mn, sc);
            ImGui::Text("Play: %.0fh %02.0fm %02.0fs", hr, mn, sc);
        }
    }
    ImGui::End();
}

/* ── reshade_overlay event — fires between NewFrame/EndFrame EVERY frame ─ */

static void on_reshade_overlay(effect_runtime *runtime)
{
    // Try to connect to emu on every frame until successful
    if (!s_bridge_ok) {
        try_connect_bridge();
        if (!s_bridge_ok) return;
    }

    update_fps();

    // Always render notifications and stats (even when ReShade overlay is closed)
    render_notifications(runtime);
    render_stats_hud();

    // Toggle main overlay with our own hotkey (Shift+Tab)
    if (runtime->is_key_down(VK_SHIFT) && runtime->is_key_pressed(VK_TAB)) {
        s_show_main_overlay = !s_show_main_overlay;
    }

    // Main overlay window
    if (s_show_main_overlay) {
        render_main_overlay(runtime);
    }
}

/* ── Main overlay window (rendered when user toggles with Shift+Tab) ──── */

static void render_main_overlay(effect_runtime *runtime)
{
    if (!s_bridge_ok || !s_bridge.GetState) return;

    GSE_OverlayState state{};
    s_bridge.GetState(&state);

    auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("GSE Overlay##main", &s_show_main_overlay, flags)) {
        ImGui::End();
        return;
    }

    // ── Toolbar ──
    ImGui::Text("GSE Overlay  |  %s  |  AppID: %u", state.username, state.app_id);
    ImGui::SameLine();
    if (ImGui::Button("Achievements"))
        s_show_achievements = !s_show_achievements;
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        s_show_main_overlay = false;

    ImGui::Separator();

    // ── Info panel ──
    ImGui::TextDisabled("Emu build  : %s", state.build_string);
    ImGui::TextDisabled("Build date : %s", state.build_date);
    ImGui::TextDisabled("Rendering  : ReShade addon (no ingame_overlay)");

    // Display info
    if (s_bridge.GetDisplayInfo) {
        GSE_DisplayInfo displays[8];
        int dcount = s_bridge.GetDisplayInfo(displays, 8);
        for (int d = 0; d < dcount; ++d) {
            auto &di = displays[d];
            const char *hdr_st = di.hdr_enabled ? "HDR ON" : (di.hdr_supported ? "HDR supported" : "SDR");
            ImGui::TextDisabled("Display %d  : %s  |  %s  |  %dbpc", d+1, di.name, hdr_st, di.bpc);
            ImGui::TextDisabled("           : gamut: %s  |  TF: %s  |  range: %s  |  enc: %s",
                di.gamut, di.transfer, di.range, di.encoding);
        }
    }

    ImGui::Separator();

    // ── Warnings ──
    if (state.warn_local_save) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        ImGui::TextWrapped("WARNING: Using local save. Achievements and stats are stored locally "
            "and will not sync with Steam servers.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    if (state.warn_bad_appid) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("WARNING: App ID is 0. This may cause issues with some features.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // ── Achievement list ──
    if (s_show_achievements) {
        render_achievement_list(runtime);
    }

    ImGui::End();
}

/* ── Achievement list ─────────────────────────────────────────────────── */

static void render_achievement_list(effect_runtime *runtime)
{
    if (!s_bridge.GetAchievements || !s_bridge.GetAchievementCount) return;

    int total = s_bridge.GetAchievementCount();
    if (total <= 0) {
        ImGui::Text("No achievements found.");
        return;
    }

    // Clamp to reasonable max for stack allocation
    int fetch = total;
    if (fetch > 2048) fetch = 2048;

    std::vector<GSE_Achievement> achs(fetch);
    int count = s_bridge.GetAchievements(achs.data(), fetch);

    // Count achieved
    int achieved_count = 0;
    for (int i = 0; i < count; ++i)
        if (achs[i].achieved) achieved_count++;

    ImGui::Text("Achievements: %d / %d unlocked", achieved_count, count);
    ImGui::Separator();

    // Scrollable child
    if (ImGui::BeginChild("##ach_list", ImVec2(0, ImGui::GetContentRegionAvail().y - 10), true)) {
        for (int i = 0; i < count; ++i) {
            auto &a = achs[i];

            ImGui::PushID(i);

            // Icon placeholder (no GPU upload in this initial version — just a colored square)
            ImVec4 icon_col = a.achieved ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, icon_col);
            ImGui::Button(a.achieved ? "OK" : "  ", ImVec2(32, 32));
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Text
            ImGui::BeginGroup();
            if (a.achieved)
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", a.title);
            else
                ImGui::Text("%s", a.title);

            if (!a.hidden || a.achieved)
                ImGui::TextDisabled("%s", a.description);
            else
                ImGui::TextDisabled("(Hidden achievement)");

            // Progress bar
            if (a.max_progress > 0 && !a.achieved) {
                float frac = (float)a.progress / (float)a.max_progress;
                char buf[64];
                snprintf(buf, sizeof(buf), "%u / %u", a.progress, a.max_progress);
                ImGui::ProgressBar(frac, ImVec2(200, 0), buf);
            }

            // Global percentage
            if (a.global_percent >= 0.0f)
                ImGui::TextDisabled("%.1f%% of players", a.global_percent);

            ImGui::EndGroup();
            ImGui::Separator();

            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

/* ── ReShade settings overlay tab (shown inside ReShade's overlay panel) ─ */

static void draw_settings_overlay(effect_runtime *runtime)
{
    if (!s_bridge_ok) {
        ImGui::Text("GSE bridge not connected. Is the emu loaded?");
        if (ImGui::Button("Retry"))
            try_connect_bridge();
        return;
    }

    GSE_OverlayState state{};
    s_bridge.GetState(&state);

    ImGui::Text("GSE Overlay v%u  |  App %u  |  %s", state.abi_version, state.app_id, state.username);
    ImGui::Separator();

    // Notification settings
    if (ImGui::CollapsingHeader("Notifications", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (s_bridge.GetOption && s_bridge.SetOption) {
            bool ach_notif = s_bridge.GetOption(GSE_OPT_ACH_NOTIF_ENABLE) != 0;
            if (ImGui::Checkbox("Achievement notifications", &ach_notif))
                s_bridge.SetOption(GSE_OPT_ACH_NOTIF_ENABLE, ach_notif);

            bool prog_notif = s_bridge.GetOption(GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE) != 0;
            if (ImGui::Checkbox("Achievement progress", &prog_notif))
                s_bridge.SetOption(GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE, prog_notif);

            bool friend_notif = s_bridge.GetOption(GSE_OPT_FRIEND_NOTIF_ENABLE) != 0;
            if (ImGui::Checkbox("Friend notifications", &friend_notif))
                s_bridge.SetOption(GSE_OPT_FRIEND_NOTIF_ENABLE, friend_notif);
        }
    }

    // Warning settings
    if (ImGui::CollapsingHeader("Warnings")) {
        if (s_bridge.GetOption && s_bridge.SetOption) {
            bool dis_all = s_bridge.GetOption(GSE_OPT_DISABLE_ALL_WARNINGS) != 0;
            if (ImGui::Checkbox("Disable all warnings", &dis_all))
                s_bridge.SetOption(GSE_OPT_DISABLE_ALL_WARNINGS, dis_all);

            bool dis_bad_appid = s_bridge.GetOption(GSE_OPT_DISABLE_BAD_APPID_WARNING) != 0;
            if (ImGui::Checkbox("Disable bad AppID warning", &dis_bad_appid))
                s_bridge.SetOption(GSE_OPT_DISABLE_BAD_APPID_WARNING, dis_bad_appid);

            bool dis_local_save = s_bridge.GetOption(GSE_OPT_DISABLE_LOCAL_SAVE_WARNING) != 0;
            if (ImGui::Checkbox("Disable local save warning", &dis_local_save))
                s_bridge.SetOption(GSE_OPT_DISABLE_LOCAL_SAVE_WARNING, dis_local_save);
        }
    }

    // Display info
    if (ImGui::CollapsingHeader("Display Info")) {
        if (s_bridge.GetDisplayInfo) {
            GSE_DisplayInfo displays[8];
            int dcount = s_bridge.GetDisplayInfo(displays, 8);
            for (int d = 0; d < dcount; ++d) {
                auto &di = displays[d];
                ImGui::Text("Display %d: %s", d+1, di.name);
                ImGui::Text("  HDR: %s | BPC: %d | Gamut: %s",
                    di.hdr_enabled ? "ON" : (di.hdr_supported ? "Available" : "No"),
                    di.bpc, di.gamut);
                ImGui::Text("  TF: %s | Range: %s | Encoding: %s",
                    di.transfer, di.range, di.encoding);
            }
            if (dcount == 0)
                ImGui::TextDisabled("No display info available");
        }

        if (s_bridge.GetSDRWhiteScale) {
            float scale = s_bridge.GetSDRWhiteScale();
            ImGui::Text("SDR white scale: %.2f", scale);
        }
    }
}

/* ── DLL entry point ──────────────────────────────────────────────────── */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;

        // Device lifecycle for texture management
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);

        // The reshade_overlay event fires between ImGui::NewFrame and EndFrame
        // EVERY frame, regardless of whether the ReShade overlay panel is open.
        // This is where we render notifications and the main overlay.
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);

        // Register a settings tab inside ReShade's overlay panel
        reshade::register_overlay("GSE Overlay Settings", draw_settings_overlay);

        // Try to connect to emu immediately
        try_connect_bridge();

        reshade::log::message(reshade::log::level::info, "GSE overlay addon loaded");
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(hModule);
        break;
    }

    return TRUE;
}
