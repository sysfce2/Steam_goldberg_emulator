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
#include <numeric>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "../libs/stb/stb_image.h"

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
static void render_achievement_list();
static void render_friends_list();

/* ── Overlay toggle state ─────────────────────────────────────────────── */

static bool s_show_main_overlay = false;
static bool s_show_achievements = false;
static bool s_show_settings     = false;
static bool s_show_user_info    = false;
static bool s_show_sce_browser  = false;

/* ── SCE browser state ────────────────────────────────────────────────── */

static std::string s_sce_storage_path;     // cached base path for sce_assets/
static std::string s_sce_preview_key;      // full file path of currently previewed image
static std::vector<std::string> s_sce_preview_nav_keys;  // sibling keys for prev/next
static int s_sce_tex_per_frame = 0;        // per-frame texture load cap counter
static constexpr int MAX_SCE_TEX_PER_FRAME = 4;

/* ── Native overlay color constants (matching steam_overlay.cpp) ──────── */

// Notification background (dark grayish-blue)
static const ImVec4 COL_NOTIF_BG        = ImVec4(0.12f, 0.14f, 0.21f, 1.0f);
// Main overlay window background
static const ImVec4 COL_MAIN_BG         = ImVec4(0.12f, 0.11f, 0.11f, 0.55f);
// Button / frame / element color (slate blue)
static const ImVec4 COL_ELEMENT         = ImVec4(0.30f, 0.32f, 0.40f, 1.0f);
// Element hovered (brighter blue)
static const ImVec4 COL_ELEMENT_HOVER   = ImVec4(0.278f, 0.393f, 0.602f, 1.0f);
// Stats HUD background
static const ImVec4 COL_STATS_BG        = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
// Stats HUD text (retro arcade yellow)
static const ImVec4 COL_STATS_TEXT      = ImVec4(0.8f, 0.7f, 0.0f, 1.0f);
// Achievement status colors
static const ImU32  COL_ACH_ACHIEVED    = IM_COL32(0, 220, 0, 255);     // Green checkmark
static const ImU32  COL_ACH_PROGRESS    = IM_COL32(255, 180, 0, 255);   // Orange arrow
static const ImU32  COL_ACH_LOCKED      = IM_COL32(220, 0, 0, 255);     // Red X
// DLC group header colors
static const ImVec4 COL_DLC_HEADER         = ImVec4(0.20f, 0.30f, 0.45f, 0.80f);
static const ImVec4 COL_DLC_HEADER_HOVER   = ImVec4(0.25f, 0.38f, 0.55f, 0.90f);
static const ImVec4 COL_DLC_HEADER_ACTIVE  = ImVec4(0.30f, 0.45f, 0.65f, 1.00f);

// Default icon size matching native
static constexpr float ICON_SIZE = 64.0f;
// Default notification rounding
static constexpr float NOTIF_ROUNDING = 10.0f;
// Notification width as fraction of screen
static constexpr float NOTIF_WIDTH_FRAC = 0.25f;
// Notification margin from screen edges
static constexpr float NOTIF_MARGIN = 5.0f;
// Animation duration in ms
static constexpr int64_t ANIM_DURATION_MS = 350;

/* ── Current device pointer (set each frame in on_reshade_overlay) ─────── */

static device *s_current_device = nullptr;

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

static void get_playtime(int &hr, int &min, int &sec)
{
    auto elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_start_time).count();
    hr  = elapsed / 3600;
    min = (elapsed % 3600) / 60;
    sec = elapsed % 60;
}

/* ── Helper: apply native overlay style colors ────────────────────────── */

static int apply_global_style_colors()
{
    int count = 0;
    auto push = [&](ImGuiCol idx, const ImVec4 &col) {
        ImGui::PushStyleColor(idx, col);
        ++count;
    };
    push(ImGuiCol_WindowBg, COL_MAIN_BG);
    push(ImGuiCol_TitleBgActive, COL_ELEMENT);
    push(ImGuiCol_Button, COL_ELEMENT);
    push(ImGuiCol_ButtonHovered, COL_ELEMENT_HOVER);
    push(ImGuiCol_FrameBg, COL_ELEMENT);
    push(ImGuiCol_FrameBgHovered, COL_ELEMENT_HOVER);
    push(ImGuiCol_ResizeGrip, COL_ELEMENT);
    push(ImGuiCol_ResizeGripHovered, COL_ELEMENT_HOVER);
    push(ImGuiCol_HeaderHovered, COL_ELEMENT_HOVER);
    return count;
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

    // Cache the SCE storage path
    if (s_bridge.GetSceStoragePath) {
        char path_buf[1024]{};
        s_bridge.GetSceStoragePath(path_buf, sizeof(path_buf));
        s_sce_storage_path = path_buf;
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
    if (s_current_device == dev)
        s_current_device = nullptr;
    auto *data = dev->get_private_data<addon_device_data>();
    if (data) {
        for (auto &[key, icon] : data->icon_cache)
            free_icon(dev, icon);
        dev->destroy_private_data<addon_device_data>();
    }
}

/* ── Helper: get or upload a cached icon by key ───────────────────────── */

static const IconTexture *get_or_upload_icon(const char *key, const uint8_t *pixels, int w, int h)
{
    if (!s_current_device || !pixels || w <= 0 || h <= 0 || !key || !key[0])
        return nullptr;

    auto *data = s_current_device->get_private_data<addon_device_data>();
    if (!data) return nullptr;

    auto it = data->icon_cache.find(key);
    if (it != data->icon_cache.end() && it->second.valid)
        return &it->second;

    IconTexture icon = upload_icon(s_current_device, pixels, w, h);
    if (!icon.valid) return nullptr;

    auto &stored = data->icon_cache[key];
    stored = icon;
    return &stored;
}

/* ── Helper: load an image from disk (stb_image) and upload to GPU ────── */

static const IconTexture *get_or_load_image(const std::string &file_path)
{
    if (!s_current_device || file_path.empty()) return nullptr;

    auto *data = s_current_device->get_private_data<addon_device_data>();
    if (!data) return nullptr;

    auto it = data->icon_cache.find(file_path);
    if (it != data->icon_cache.end()) {
        return it->second.valid ? &it->second : nullptr;
    }

    // Respect per-frame load cap
    if (s_sce_tex_per_frame >= MAX_SCE_TEX_PER_FRAME) return nullptr;
    ++s_sce_tex_per_frame;

    // Insert a placeholder (even on failure) so we don't retry every frame
    IconTexture placeholder{};
    data->icon_cache[file_path] = placeholder;

    int w = 0, h = 0;
    uint8_t *pixels = stbi_load(file_path.c_str(), &w, &h, nullptr, 4);
    if (!pixels) return nullptr;

    IconTexture icon = upload_icon(s_current_device, pixels, w, h);
    stbi_image_free(pixels);

    if (!icon.valid) return nullptr;

    data->icon_cache[file_path] = icon;
    return &data->icon_cache[file_path];
}

/* ── Helper: load a preview image immediately (no frame cap) ──────────── */

static const IconTexture *load_preview_image(const std::string &file_path)
{
    if (!s_current_device || file_path.empty()) return nullptr;

    auto *data = s_current_device->get_private_data<addon_device_data>();
    if (!data) return nullptr;

    auto it = data->icon_cache.find(file_path);
    if (it != data->icon_cache.end())
        return it->second.valid ? &it->second : nullptr;

    int w = 0, h = 0;
    uint8_t *pixels = stbi_load(file_path.c_str(), &w, &h, nullptr, 4);
    if (!pixels) {
        data->icon_cache[file_path] = IconTexture{};
        return nullptr;
    }

    IconTexture icon = upload_icon(s_current_device, pixels, w, h);
    stbi_image_free(pixels);

    if (!icon.valid) {
        data->icon_cache[file_path] = IconTexture{};
        return nullptr;
    }

    data->icon_cache[file_path] = icon;
    return &data->icon_cache[file_path];
}

/* ── Helper: free all SCE textures from the cache ─────────────────────── */

static void free_sce_textures()
{
    if (!s_current_device) return;
    auto *data = s_current_device->get_private_data<addon_device_data>();
    if (!data) return;

    // Remove entries that start with the SCE storage path prefix
    for (auto it = data->icon_cache.begin(); it != data->icon_cache.end(); ) {
        if (it->first.size() > 4 && (it->first.find("sce_assets") != std::string::npos ||
            it->first.find("Series ") != std::string::npos)) {
            free_icon(s_current_device, it->second);
            it = data->icon_cache.erase(it);
        } else {
            ++it;
        }
    }
}

/* ── SCE browser helper: sanitize filename (same as native) ───────────── */

static std::string sce_sanitize(const char *name)
{
    std::string s(name);
    for (char &c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    if (s.size() > 48) s.resize(48);
    return s;
}

/* ── SCE browser helper: extract file extension from URL ──────────────── */

static std::string sce_url_ext(const char *url)
{
    if (!url || !url[0]) return ".png";
    std::string u(url);
    size_t slash = u.rfind('/');
    std::string name = (slash != std::string::npos) ? u.substr(slash + 1) : u;
    size_t q = name.find('?');
    if (q != std::string::npos) name.resize(q);
    size_t dot = name.rfind('.');
    return (dot != std::string::npos) ? name.substr(dot) : ".png";
}

/* ── SCE browser helper: type subfolder name (same as native) ─────────── */

static const char *sce_type_subfolder(int tidx)
{
    static constexpr const char *dirs[] = {
        "cards", "foil_cards", "booster_packs",
        "badges", "foil_badges", "emoticons",
        "backgrounds", "animated_backgrounds", "animated_mini_backgrounds",
        "profiles", "avatar_frames", "animated_avatars",
        "animated_stickers", "startup_movies"
    };
    return (tidx >= 0 && tidx < 14) ? dirs[tidx] : "misc";
}

/* ── SCE browser helper: rarity colour ────────────────────────────────── */

static ImVec4 sce_rarity_color(const char *rarity)
{
    if (!rarity) return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    if (strcmp(rarity, "Uncommon") == 0) return ImVec4(0.20f, 0.85f, 0.20f, 1.0f);
    if (strcmp(rarity, "Rare") == 0)     return ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
    return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
}

/* ── Notification rendering (always visible, via reshade_overlay event) ─ */

// Animation factor: 0.0 = fully visible, 1.0 = fully off-screen
static float animate_factor(int64_t elapsed_ms, int64_t display_duration_ms)
{
    // Phase 1: slide in
    if (elapsed_ms < ANIM_DURATION_MS) {
        return 1.0f - (float)elapsed_ms / (float)ANIM_DURATION_MS;
    }
    // Phase 2: steady
    if (elapsed_ms < ANIM_DURATION_MS + display_duration_ms) {
        return 0.0f;
    }
    // Phase 3: slide out
    int64_t out_elapsed = elapsed_ms - ANIM_DURATION_MS - display_duration_ms;
    if (out_elapsed < ANIM_DURATION_MS) {
        return (float)out_elapsed / (float)ANIM_DURATION_MS;
    }
    return 1.0f;
}

// Accumulated Y offsets per notification region (reset each frame)
struct NotifCoords {
    float top_left = 0, top_center = 0, top_right = 0;
    float bot_left = 0, bot_center = 0, bot_right = 0;
};

static void render_notifications(effect_runtime *runtime)
{
    if (!s_bridge_ok || !s_bridge.GetNotifications) return;

    GSE_Notification notifs[16];
    int count = s_bridge.GetNotifications(notifs, 16);
    if (count <= 0) return;

    auto &io = ImGui::GetIO();
    float screen_w = io.DisplaySize.x;
    float screen_h = io.DisplaySize.y;

    float notif_w = screen_w * NOTIF_WIDTH_FRAC;
    if (notif_w < 300.0f) notif_w = 300.0f;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, NOTIF_ROUNDING);

    NotifCoords coords{};

    for (int i = 0; i < count; ++i) {
        auto &n = notifs[i];

        int64_t elapsed = now_ms - n.start_time_ms;
        if (elapsed < 0) elapsed = 0;

        // Total lifetime = anim_in + display + anim_out
        int64_t total = n.duration_ms + ANIM_DURATION_MS * 2;
        if (n.duration_ms > 0 && elapsed > total) {
            if (s_bridge.ExpireNotification)
                s_bridge.ExpireNotification(n.id);
            continue;
        }

        int64_t display_dur = n.duration_ms > 0 ? n.duration_ms : 7000;
        float factor = animate_factor(elapsed, display_dur);
        float alpha = 1.0f - factor;

        // Compute notification height estimate
        float font_size = ImGui::GetFontSize();
        float title_h = font_size + ImGui::GetStyle().ItemSpacing.y;
        float row_h = ICON_SIZE;  // icon row
        float padding = ImGui::GetStyle().WindowPadding.y * 2.0f;
        float notif_h = title_h + row_h + padding;
        bool has_progress = (n.type == GSE_NOTIF_ACHIEVEMENT_PROG ||
                            (n.type == GSE_NOTIF_ACHIEVEMENT && !n.ach_achieved)) &&
                            n.ach_max_progress > 0;
        if (has_progress) notif_h += font_size + ImGui::GetStyle().WindowPadding.y;

        // Get position preference
        int notif_pos = GSE_NOTIF_POS_BOT_RIGHT;  // default for achievements
        if (n.type == GSE_NOTIF_INVITE)
            notif_pos = GSE_NOTIF_POS_TOP_RIGHT;
        else if (s_bridge.GetOption)
            notif_pos = s_bridge.GetOption(GSE_OPT_NOTIF_POSITION);

        // Position calculation matching native overlay
        float slide = factor;
        float x = 0, y = 0;
        switch (notif_pos) {
        case GSE_NOTIF_POS_TOP_LEFT: {
            float anim_off = slide * notif_w;
            x = NOTIF_MARGIN - anim_off;
            y = coords.top_left + NOTIF_MARGIN;
            coords.top_left = y + notif_h;
        } break;
        case GSE_NOTIF_POS_TOP_CENTER: {
            float anim_off = slide * notif_h;
            x = (screen_w - notif_w) * 0.5f;
            y = coords.top_center + NOTIF_MARGIN - anim_off;
            coords.top_center = y + notif_h;
        } break;
        case GSE_NOTIF_POS_TOP_RIGHT: {
            float anim_off = slide * notif_w;
            x = screen_w - notif_w - NOTIF_MARGIN + anim_off;
            y = coords.top_right + NOTIF_MARGIN;
            coords.top_right = y + notif_h;
        } break;
        case GSE_NOTIF_POS_BOT_LEFT: {
            float anim_off = slide * notif_w;
            x = NOTIF_MARGIN - anim_off;
            y = screen_h - coords.bot_left - NOTIF_MARGIN - notif_h;
            coords.bot_left = screen_h - y;
        } break;
        case GSE_NOTIF_POS_BOT_CENTER: {
            float anim_off = slide * notif_h;
            x = (screen_w - notif_w) * 0.5f;
            y = screen_h - coords.bot_center - NOTIF_MARGIN - notif_h + anim_off;
            coords.bot_center = screen_h - y;
        } break;
        default:
        case GSE_NOTIF_POS_BOT_RIGHT: {
            float anim_off = slide * notif_w;
            x = screen_w - notif_w - NOTIF_MARGIN + anim_off;
            y = screen_h - coords.bot_right - NOTIF_MARGIN - notif_h;
            coords.bot_right = screen_h - y;
        } break;
        }

        ImGui::SetNextWindowPos(ImVec2(x, y));
        ImGui::SetNextWindowSize(ImVec2(notif_w, 0));

        char win_id[64];
        snprintf(win_id, sizeof(win_id), "##gse_notif_%d", n.id);

        // Some notification types are non-interactive (like native overlay)
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoSavedSettings;

        switch (n.type) {
        case GSE_NOTIF_ACHIEVEMENT:
        case GSE_NOTIF_ACHIEVEMENT_PROG:
        case GSE_NOTIF_AUTO_ACCEPT_INVITE:
        case GSE_NOTIF_MESSAGE:
            flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;
        default:
            break;
        }

        // Push native notification colors
        ImGui::PushStyleColor(ImGuiCol_WindowBg, COL_NOTIF_BG);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, alpha));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

        if (ImGui::Begin(win_id, nullptr, flags)) {
            switch (n.type) {
            case GSE_NOTIF_ACHIEVEMENT:
            case GSE_NOTIF_ACHIEVEMENT_PROG: {
                bool is_unlock = (n.type == GSE_NOTIF_ACHIEVEMENT) && n.ach_achieved;

                // Title (centered)
                ImGui::Text("%s", n.ach_title[0] ? n.ach_title : "Achievement");

                // Icon + description table (matching native layout)
                bool has_icon = (n.ach_icon_pixels != nullptr && n.ach_icon_w > 0 && n.ach_icon_h > 0);
                if (has_icon && ImGui::BeginTable("##ach_notif_tbl", 2)) {
                    ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, ICON_SIZE);
                    ImGui::TableSetupColumn("text");
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, ICON_SIZE);

                    ImGui::TableSetColumnIndex(0);
                    {
                        // Try to render real icon texture
                        char icon_key[320];
                        snprintf(icon_key, sizeof(icon_key), "notif_%d", n.id);
                        const IconTexture *icon = get_or_upload_icon(icon_key, n.ach_icon_pixels, n.ach_icon_w, n.ach_icon_h);
                        if (icon) {
                            ImGui::Image(ImTextureRef(icon->srv.handle), ImVec2(ICON_SIZE, ICON_SIZE));
                        } else {
                            // Fallback: colored placeholder
                            ImVec4 icon_col = is_unlock
                                ? ImVec4(0.2f, 0.8f, 0.2f, alpha)
                                : ImVec4(0.5f, 0.5f, 0.5f, alpha);
                            ImGui::PushStyleColor(ImGuiCol_Button, icon_col);
                            ImGui::Button(is_unlock ? u8"\u2713" : "?", ImVec2(ICON_SIZE, ICON_SIZE));
                            ImGui::PopStyleColor();
                        }
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", n.message);

                    ImGui::EndTable();
                } else {
                    ImGui::TextWrapped("%s", n.message);
                }

                // Progress bar for in-progress achievements
                if (has_progress) {
                    float frac = (float)n.ach_progress / (float)n.ach_max_progress;
                    char prog_buf[32];
                    snprintf(prog_buf, sizeof(prog_buf), "%u/%u", n.ach_progress, n.ach_max_progress);
                    ImGui::ProgressBar(frac, ImVec2(-1, ImGui::GetFontSize()), prog_buf);
                }
                break;
            }
            case GSE_NOTIF_INVITE:
                ImGui::TextWrapped("%s", n.message);
                if (ImGui::Button("Join")) {
                    // Expire the notification on accept
                    if (s_bridge.ExpireNotification)
                        s_bridge.ExpireNotification(n.id);
                }
                break;
            case GSE_NOTIF_AUTO_ACCEPT_INVITE:
                ImGui::TextWrapped("%s", n.message);
                break;
            default:
                ImGui::TextWrapped("%s", n.message);
                break;
            }
        }
        ImGui::End();

        ImGui::PopStyleVar();   // Alpha
        ImGui::PopStyleColor(3); // WindowBg, Border, Text
    }

    ImGui::PopStyleVar(); // WindowRounding
}

/* ── Stats HUD (always visible when enabled, matching native style) ────── */

static void render_stats_hud()
{
    if (!s_bridge_ok || !s_bridge.GetState) return;

    GSE_OverlayState state{};
    s_bridge.GetState(&state);

    bool any_stats = state.show_fps || state.show_frametime || state.show_playtime;
    if (!any_stats) return;

    // Build the stats line matching native format: "FPS: XX | Frametime: X.Xms | Playtime: HH:MM:SS"
    char stats_text[256] = {};
    bool need_sep = false;

    if (state.show_fps) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "FPS: %2.0f", s_addon_fps);
        strcat(stats_text, tmp);
        need_sep = true;
    }
    if (state.show_frametime) {
        if (need_sep) strcat(stats_text, " | ");
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "Frametime: %.1fms", s_addon_frametime);
        strcat(stats_text, tmp);
        need_sep = true;
    }
    if (state.show_playtime) {
        if (need_sep) strcat(stats_text, " | ");
        int hr, mn, sc;
        get_playtime(hr, mn, sc);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "Playtime: %02d:%02d:%02d", hr % 24, mn, sc);
        strcat(stats_text, tmp);
    }

    // Calculate text size to auto-fit the window
    ImVec2 text_sz = ImGui::CalcTextSize(stats_text);
    ImVec2 padding = ImGui::GetStyle().WindowPadding;
    ImVec2 box_sz = ImVec2(text_sz.x + padding.x * 2.0f, text_sz.y + padding.y * 2.0f);

    // Position: use the stats_pos from bridge state (normalized 0..1), default top-left
    // The anchor point within the stats box moves with the position setting
    float pos_x = 0.0f, pos_y = 0.0f;  // default: top-left
    // TODO: expose stats_pos_x/y through the bridge when available
    float anchor_x = box_sz.x * pos_x;
    float anchor_y = box_sz.y * pos_y;
    float screen_x = ImGui::GetIO().DisplaySize.x * pos_x - anchor_x;
    float screen_y = ImGui::GetIO().DisplaySize.y * pos_y - anchor_y;

    ImGui::SetNextWindowPos(ImVec2(screen_x, screen_y));
    ImGui::SetNextWindowSize(box_sz);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, NOTIF_ROUNDING);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, COL_STATS_BG);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_STATS_TEXT);

    if (ImGui::Begin("##gse_stats", nullptr, flags)) {
        ImGui::TextUnformatted(stats_text);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

/* ── reshade_overlay event — fires between NewFrame/EndFrame EVERY frame ─ */

static void on_reshade_overlay(effect_runtime *runtime)
{
    // Try to connect to emu on every frame until successful
    if (!s_bridge_ok) {
        try_connect_bridge();
        if (!s_bridge_ok) return;
    }

    // Store device pointer for icon upload during this frame
    s_current_device = runtime->get_device();
    s_sce_tex_per_frame = 0;  // reset per-frame load cap

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

// State for friend context menu
static uint64_t s_ctx_friend_id = 0;

static void render_main_overlay(effect_runtime *runtime)
{
    if (!s_bridge_ok || !s_bridge.GetState) return;

    GSE_OverlayState state{};
    s_bridge.GetState(&state);

    auto &io = ImGui::GetIO();

    // Window title matching native: "Ingame Overlay project - Nemirtingas (Renderer: ReShade Addon)"
    char windowTitle[512];
    snprintf(windowTitle, sizeof(windowTitle),
        "Ingame Overlay project - Nemirtingas (Renderer: ReShade Addon)##gse_main");

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    // Apply native style colors
    int style_colors = apply_global_style_colors();

    bool show = true;
    if (!ImGui::Begin(windowTitle, &show,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::End();
        ImGui::PopStyleColor(style_colors);
        if (!show) s_show_main_overlay = false;
        return;
    }
    if (!show) s_show_main_overlay = false;

    // ── User Info (togglable, matching native LabelText format) ──
    if (s_show_user_info) {
        ImGui::LabelText("##playinglabel", "%s  (ID: %llu)  playing AppID %u",
            state.username, (unsigned long long)state.steam_id, state.app_id);
    }

    ImGui::Spacing();

    // ── Button Bar (matching native order exactly) ──
    ImGui::SameLine();
    if (ImGui::Button("Toggle User Info"))
        s_show_user_info = !s_show_user_info;

    ImGui::SameLine();
    if (ImGui::Button("Show Achievements"))
        s_show_achievements = !s_show_achievements;

    ImGui::SameLine();
    if (ImGui::Button("Test Achievement")) {
        if (s_bridge.TestAchievement)
            s_bridge.TestAchievement();
    }

    ImGui::SameLine();
    if (ImGui::Button("Copy ID")) {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)state.steam_id);
        ImGui::SetClipboardText(id_str);
    }

    ImGui::SameLine();
    if (ImGui::Button("Settings"))
        s_show_settings = !s_show_settings;

    // ── SCE buttons (matching native: only shown when catalog data is present) ──
    if (s_bridge.GetSceStatus) {
        GSE_SceStatus sce{};
        s_bridge.GetSceStatus(&sce);

        if (sce.data_available) {
            ImGui::SameLine();
            if (!sce.downloading) {
                if (ImGui::Button("Download SCE Assets") && s_bridge.RequestSceDownload)
                    s_bridge.RequestSceDownload();
                ImGui::SameLine();
                if (ImGui::Button(s_show_sce_browser ? "[SCE Assets]" : "Browse SCE Assets"))
                    s_show_sce_browser = !s_show_sce_browser;
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("Downloading SCE Assets...");
                ImGui::EndDisabled();
            }

            // ── SCE Download Progress floating window (matching native: 25% wide, centered) ──
            if (sce.downloading) {
                float win_w = io.DisplaySize.x * 0.25f;
                ImGui::SetNextWindowSize(ImVec2(win_w, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowPos(
                    ImVec2(io.DisplaySize.x * 0.5f - win_w * 0.5f, io.DisplaySize.y * 0.08f),
                    ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.92f);
                ImGui::Begin("SCE Download Progress##sce_dl", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_AlwaysAutoResize);

                if (sce.grand_total > 0) {
                    uint32_t grand_done = sce.grand_downloaded + sce.grand_skipped;
                    char total_lbl[128]{};
                    snprintf(total_lbl, sizeof(total_lbl),
                        "Total: %u downloaded, %u cached of %u",
                        sce.grand_downloaded, sce.grand_skipped, sce.grand_total);
                    float frac = (float)grand_done / (float)sce.grand_total;
                    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), total_lbl);
                } else {
                    ImGui::TextUnformatted("Collecting asset URLs...");
                }

                ImGui::Spacing();
                for (int i = 0; i < GSE_SCE_NUM_TYPES; ++i) {
                    uint32_t tot = sce.types[i].total;
                    if (tot == 0) continue;
                    uint32_t cur = sce.types[i].current;
                    uint32_t dl  = sce.types[i].downloaded;
                    const char *label = sce.type_labels[i] ? sce.type_labels[i] : "Unknown";
                    char lbl[128]{};
                    snprintf(lbl, sizeof(lbl), "%s: %u of %u (%u new)",
                        label, cur, tot, dl);
                    ImGui::ProgressBar((float)cur / (float)tot, ImVec2(-1.0f, 0.0f), lbl);
                }

                ImGui::End();
            }
        }
    }

    // FPS / Frametime / Playtime checkboxes (matching native layout)
    ImGui::Spacing(); ImGui::Spacing();
    ImGui::SameLine();
    {
        bool fps_on = state.show_fps != 0;
        bool ft_on  = state.show_frametime != 0;
        bool pt_on  = state.show_playtime != 0;
        if (ImGui::Checkbox("FPS", &fps_on) && s_bridge.SetOption)
            s_bridge.SetOption(GSE_OPT_SHOW_FPS, fps_on ? 1 : 0);
        ImGui::SameLine();
        if (ImGui::Checkbox("Frametime", &ft_on) && s_bridge.SetOption)
            s_bridge.SetOption(GSE_OPT_SHOW_FRAMETIME, ft_on ? 1 : 0);
        ImGui::SameLine();
        if (ImGui::Checkbox("Playtime", &pt_on) && s_bridge.SetOption)
            s_bridge.SetOption(GSE_OPT_SHOW_PLAYTIME, pt_on ? 1 : 0);
    }

    // ── Rendering Info Panel (matching native layout) ──
    ImGui::Spacing();
    ImGui::Separator();
    {
        ImGui::TextDisabled("API        : ReShade Addon  (no ingame_overlay)");
        ImGui::TextDisabled("Emu build  : %s", state.build_string);
        ImGui::TextDisabled("Build date : %s", state.build_date);

        // Per-display info
        if (s_bridge.GetDisplayInfo) {
            GSE_DisplayInfo displays[4];
            int dcount = s_bridge.GetDisplayInfo(displays, 4);
            if (dcount == 0) {
                ImGui::TextDisabled("Display    : N/A");
            } else {
                for (int d = 0; d < dcount; ++d) {
                    auto &di = displays[d];
                    const char *hdr_st;
                    if (!di.hdr_supported)       hdr_st = "SDR only";
                    else if (di.force_disabled)  hdr_st = "HDR suppressed";
                    else if (di.wide_color && !di.hdr_enabled) hdr_st = "WCG (no HDR)";
                    else if (di.hdr_enabled)     hdr_st = "HDR ON";
                    else                         hdr_st = "HDR OFF (supported)";
                    char bpc_buf[8] = "?";
                    if (di.bpc > 0) snprintf(bpc_buf, sizeof(bpc_buf), "%d", di.bpc);
                    char white_buf[24] = "?";
                    if (di.sdr_white_nits >= 0)
                        snprintf(white_buf, sizeof(white_buf), "%d nits", di.sdr_white_nits);
                    ImGui::TextDisabled("Display %d  : %s  |  %s  |  %sbpc  |  SDR white: %s",
                        d + 1, di.name, hdr_st, bpc_buf, white_buf);
                    ImGui::TextDisabled("           : gamut: %s  |  TF: %s  |  range: %s  |  enc: %s",
                        di.gamut, di.transfer, di.range, di.encoding);
                }
            }
        }

        if (ImGui::SmallButton("Refresh##hdr_info")) {
            // Re-query display info (bridge re-queries each call)
        }

        if (s_bridge.GetSDRWhiteScale) {
            float scale = s_bridge.GetSDRWhiteScale();
            if (scale > 1.01f)
                ImGui::TextDisabled("HDR scale  : %.2fx  (SDR white = %d nits)", scale, (int)(scale * 80.f + 0.5f));
        }
    }
    ImGui::Separator();

    // ── Friends Section (matching native: label + Invite All button + ListBox) ──
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::LabelText("##label", "Friends");

    render_friends_list();

    ImGui::End();
    ImGui::PopStyleColor(style_colors);

    // ── Settings Window (separate Begin(), matching native) ──
    if (s_show_settings) {
        ImGui::SetNextWindowBgAlpha(1.0f);
        if (ImGui::Begin("Global Settings##gse_settings", &s_show_settings)) {
            ImGui::Text("Configure overlay behaviour below.");
            ImGui::Separator();

            if (s_bridge.GetOption && s_bridge.SetOption) {
                bool ach_notif = s_bridge.GetOption(GSE_OPT_ACH_NOTIF_ENABLE) != 0;
                if (ImGui::Checkbox("Achievement notifications", &ach_notif))
                    s_bridge.SetOption(GSE_OPT_ACH_NOTIF_ENABLE, ach_notif);

                bool prog_notif = s_bridge.GetOption(GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE) != 0;
                if (ImGui::Checkbox("Achievement progress notifications", &prog_notif))
                    s_bridge.SetOption(GSE_OPT_ACH_PROGRESS_NOTIF_ENABLE, prog_notif);

                bool friend_notif = s_bridge.GetOption(GSE_OPT_FRIEND_NOTIF_ENABLE) != 0;
                if (ImGui::Checkbox("Friend notifications", &friend_notif))
                    s_bridge.SetOption(GSE_OPT_FRIEND_NOTIF_ENABLE, friend_notif);

                ImGui::Separator();

                bool dis_all_warn = s_bridge.GetOption(GSE_OPT_DISABLE_ALL_WARNINGS) != 0;
                if (ImGui::Checkbox("Disable all warnings", &dis_all_warn))
                    s_bridge.SetOption(GSE_OPT_DISABLE_ALL_WARNINGS, dis_all_warn);

                bool dis_bad_appid = s_bridge.GetOption(GSE_OPT_DISABLE_BAD_APPID_WARNING) != 0;
                if (ImGui::Checkbox("Disable bad AppID warning", &dis_bad_appid))
                    s_bridge.SetOption(GSE_OPT_DISABLE_BAD_APPID_WARNING, dis_bad_appid);

                bool dis_local_save = s_bridge.GetOption(GSE_OPT_DISABLE_LOCAL_SAVE_WARNING) != 0;
                if (ImGui::Checkbox("Disable local save warning", &dis_local_save))
                    s_bridge.SetOption(GSE_OPT_DISABLE_LOCAL_SAVE_WARNING, dis_local_save);
            }

            ImGui::Separator();
            ImGui::Text("Restart the game to apply some settings.");
            if (ImGui::Button("Save")) {
                if (s_bridge.SetOption)
                    s_bridge.SetOption(GSE_OPT_DISABLE_ALL_WARNINGS, s_bridge.GetOption(GSE_OPT_DISABLE_ALL_WARNINGS)); // triggers save
                s_show_settings = false;
            }
        }
        ImGui::End();
    }

    // ── Warning Window (separate Begin(), matching native) ──
    {
        bool show_warning = state.warn_local_save || state.warn_bad_appid;
        if (show_warning) {
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(ImGui::GetFontSize() * 32, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowBgAlpha(1.0f);
            bool show_win = true;
            if (ImGui::Begin("Warning##gse_warn", &show_win)) {
                if (state.warn_bad_appid) {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "WARNING WARNING WARNING");
                    ImGui::TextWrapped("The AppID is 0 or invalid. This may cause issues with many features. "
                        "Make sure to set a valid AppID in the emu configuration.");
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "WARNING WARNING WARNING");
                }
                if (state.warn_local_save) {
                    ImGui::TextColored(ImVec4(1, 0.8f, 0, 1),
                        "Using local save. Achievements and stats are stored locally "
                        "and will not sync with Steam servers.");
                }
            }
            ImGui::End();
        }
    }

    // ── Achievement Window (separate Begin(), matching native) ──
    if (s_show_achievements) {
        render_achievement_list();
    }

    // ── SCE Asset Browser Window (matching native exactly) ──
    if (s_show_sce_browser && s_bridge.GetSceStatus && s_bridge.GetSceSeriesCount) {
        GSE_SceStatus sce{};
        s_bridge.GetSceStatus(&sce);

        if (!sce.data_available) {
            s_show_sce_browser = false;
            free_sce_textures();
        } else {
            // Per-type card dimensions matching native: {card_width, image_height}
            struct CardDims { float cw, ih; };
            static constexpr CardDims kDims[14] = {
                {160.f, 200.f},  // 0  cards                 (portrait)
                {160.f, 200.f},  // 1  foil_cards
                {160.f, 160.f},  // 2  booster_packs          (square)
                {160.f, 160.f},  // 3  badges
                {160.f, 160.f},  // 4  foil_badges
                {160.f, 160.f},  // 5  emoticons
                {220.f, 124.f},  // 6  backgrounds            (16:9)
                {220.f, 124.f},  // 7  animated_backgrounds
                {220.f, 124.f},  // 8  animated_mini_backgrounds
                {160.f, 160.f},  // 9  profiles
                {160.f, 160.f},  // 10 avatar_frames
                {160.f, 160.f},  // 11 animated_avatars
                {160.f, 160.f},  // 12 animated_stickers
                {220.f, 124.f},  // 13 startup_movies         (16:9)
            };

            // Tab grouping matching native
            struct SceTabGroup { const char *label; int types[4]; int ntype; };
            static constexpr SceTabGroup kTabs[] = {
                { "Cards",       { 0,  1,  2, -1}, 3 },
                { "Badges",      { 3,  4, -1, -1}, 2 },
                { "Backgrounds", { 6,  7,  8, -1}, 3 },
                { "Chat",        { 5, 12, -1, -1}, 2 },
                { "Profiles",    {11, 10,  9, -1}, 3 },
                { "Steam",       {13, -1, -1, -1}, 1 },
            };
            constexpr float CARD_GAP = 12.0f;

            auto &io = ImGui::GetIO();
            const float min_w = io.DisplaySize.x * 0.60f;
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(min_w, io.DisplaySize.y * 0.50f),
                ImVec2(8192.0f, 8192.0f));
            ImGui::SetNextWindowBgAlpha(1.0f);

            bool browser_open = s_show_sce_browser;
            if (ImGui::Begin("SCE Assets##sce_browser", &browser_open)) {
                ImGui::TextDisabled("Assets folder: %s", s_sce_storage_path.c_str());
                ImGui::Separator();
                ImGui::Spacing();

                // Get all series
                int ser_count = s_bridge.GetSceSeriesCount();
                std::vector<GSE_SceSeries> series_arr(ser_count > 64 ? 64 : ser_count);
                if (s_bridge.GetSceSeries)
                    ser_count = s_bridge.GetSceSeries(series_arr.data(), (int)series_arr.size());

                for (int si = 0; si < ser_count; ++si) {
                    auto &ser = series_arr[si];

                    // Series header
                    char ser_label[256]{};
                    if (ser.series_name[0])
                        snprintf(ser_label, sizeof(ser_label), "Series %d - %s",
                            ser.series_number, ser.series_name);
                    else
                        snprintf(ser_label, sizeof(ser_label), "Series %d", ser.series_number);

                    // Build series directory name (matching downloader)
                    char ser_num_str[8]{};
                    snprintf(ser_num_str, sizeof(ser_num_str), "%02d", ser.series_number);
                    std::string ser_dir = std::string("Series ") + ser_num_str;
                    if (ser.series_name[0])
                        ser_dir += " - " + sce_sanitize(ser.series_name);

                    if (!ImGui::CollapsingHeader(ser_label, ImGuiTreeNodeFlags_DefaultOpen))
                        continue;

                    // Get all items in this series
                    std::vector<GSE_SceItem> items(ser.item_count > 512 ? 512 : ser.item_count);
                    int item_count = 0;
                    if (s_bridge.GetSceItems)
                        item_count = s_bridge.GetSceItems(ser.series_number, items.data(), (int)items.size());

                    // Group items by type
                    std::unordered_map<int, std::vector<int>> by_type; // type -> item indices
                    for (int i = 0; i < item_count; ++i)
                        by_type[items[i].type].push_back(i);

                    char tab_bar_id[32]{};
                    snprintf(tab_bar_id, sizeof(tab_bar_id), "##tb_%d", ser.series_number);
                    if (!ImGui::BeginTabBar(tab_bar_id)) { ImGui::Spacing(); continue; }

                    for (int tgi = 0; tgi < (int)(sizeof(kTabs)/sizeof(kTabs[0])); ++tgi) {
                        auto &tg = kTabs[tgi];

                        // Count total items for this tab group
                        int tab_total = 0;
                        for (int ti = 0; ti < tg.ntype; ++ti) {
                            auto it2 = by_type.find(tg.types[ti]);
                            if (it2 != by_type.end()) tab_total += (int)it2->second.size();
                        }
                        if (tab_total == 0) continue;

                        char tab_lbl[80]{};
                        snprintf(tab_lbl, sizeof(tab_lbl), "%s (%d)##tb_%d_%d",
                            tg.label, tab_total, ser.series_number, tgi);
                        if (!ImGui::BeginTabItem(tab_lbl)) continue;

                        bool first_type = true;
                        for (int ti = 0; ti < tg.ntype; ++ti) {
                            int tidx = tg.types[ti];
                            auto it3 = by_type.find(tidx);
                            if (it3 == by_type.end() || it3->second.empty()) continue;

                            auto &item_indices = it3->second;
                            float card_w = kDims[tidx].cw;
                            float img_h  = kDims[tidx].ih;

                            // Sub-header per type
                            if (!first_type) ImGui::Spacing();
                            ImGui::TextDisabled("%s  (%zu)",
                                sce.type_labels[tidx] ? sce.type_labels[tidx] : "Unknown",
                                item_indices.size());
                            ImGui::Separator();
                            ImGui::Spacing();
                            first_type = false;

                            // Wrapping card grid
                            float avail_w = ImGui::GetContentRegionAvail().x;
                            int cols = (std::max)(1, (int)((avail_w + CARD_GAP) / (card_w + CARD_GAP)));
                            char tbl_id[32]{};
                            snprintf(tbl_id, sizeof(tbl_id), "##cg_%d_%d", ser.series_number, tidx);
                            if (ImGui::BeginTable(tbl_id, cols,
                                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody |
                                    ImGuiTableFlags_NoPadOuterX)) {
                                for (int c = 0; c < cols; ++c)
                                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, card_w);

                                int slot_idx = 0;
                                for (int idx : item_indices) {
                                    auto &item = items[idx];
                                    ++slot_idx;
                                    ImGui::TableNextColumn();

                                    bool is_bg = (tidx == 6 || tidx == 7 || tidx == 8);

                                    // Build thumbnail file path (mirrors downloader naming)
                                    const char *thumb_src = is_bg
                                        ? (item.wallpaper_url[0] ? item.wallpaper_url : item.static_img_url)
                                        : (item.icon_url[0] ? item.icon_url : item.static_img_url);
                                    std::string ext = sce_url_ext(thumb_src);
                                    std::string item_label = sce_sanitize(item.name);
                                    char prefix[8]{};
                                    snprintf(prefix, sizeof(prefix), "%02d_", slot_idx);

                                    std::string filename = is_bg
                                        ? (std::string(prefix) + "thumb_wallpaper_" + item_label + ext)
                                        : (std::string(prefix) + "icon_" + item_label + ext);
                                    std::string type_dir = sce_type_subfolder(tidx);
                                    std::string folder = s_sce_storage_path + ser_dir + "\\" + type_dir;
                                    std::string tex_path = folder + "\\" + filename;

                                    bool is_static = (ext != ".gif" && ext != ".mp4" && ext != ".webm");

                                    // Try to load the image texture
                                    const IconTexture *tex = nullptr;
                                    if (is_static)
                                        tex = get_or_load_image(tex_path);

                                    // --- Card ---
                                    ImGui::BeginGroup();

                                    // Slot / total line above image
                                    if (item.slot > 0 && item.total > 0)
                                        ImGui::TextDisabled("#%d of %d", item.slot, item.total);
                                    else if (item.slot > 0)
                                        ImGui::TextDisabled("#%d", item.slot);
                                    else
                                        ImGui::TextDisabled(" ");

                                    // Image area
                                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                                    ImVec2 p1 = ImVec2(p0.x + card_w, p0.y + img_h);
                                    ImDrawList *dl = ImGui::GetWindowDrawList();
                                    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 50, 255));

                                    char btn_id[64]{};
                                    snprintf(btn_id, sizeof(btn_id), "##bg_%d_%d_%d",
                                        ser.series_number, tidx, slot_idx);
                                    ImGui::InvisibleButton(btn_id, ImVec2(card_w, img_h));
                                    bool clicked = ImGui::IsItemClicked();

                                    if (is_static && tex && tex->valid) {
                                        // Aspect-fit (letterbox) image into card_w x img_h
                                        float sa = (tex->h > 0) ? (float)tex->w / tex->h : 1.0f;
                                        float da = card_w / img_h;
                                        float dw, dh, ox = 0.f, oy = 0.f;
                                        if (sa >= da) {
                                            dw = card_w; dh = card_w / sa;
                                            oy = (img_h - dh) * 0.5f;
                                        } else {
                                            dh = img_h; dw = img_h * sa;
                                            ox = (card_w - dw) * 0.5f;
                                        }
                                        dl->AddImage(ImTextureRef(tex->srv.handle),
                                            ImVec2(p0.x + ox, p0.y + oy),
                                            ImVec2(p0.x + ox + dw, p0.y + oy + dh));
                                        // Hover highlight
                                        if (ImGui::IsItemHovered())
                                            dl->AddRect(p0, p1, IM_COL32(200, 200, 255, 180), 0.f, 0, 2.f);
                                    } else if (!is_static) {
                                        // Animated/video: show extension badge centred
                                        const char *badge = ext.size() > 1 ? ext.c_str() + 1 : ext.c_str();
                                        ImVec2 tsz = ImGui::CalcTextSize(badge);
                                        dl->AddText(
                                            ImVec2(p0.x + (card_w - tsz.x) * 0.5f,
                                                   p0.y + (img_h  - tsz.y) * 0.5f),
                                            IM_COL32(160, 160, 160, 255), badge);
                                    }

                                    // On click: open full-size preview
                                    bool is_animated_bg = (tidx == 7 || tidx == 8);
                                    bool can_preview = is_animated_bg ? (item.wallpaper_url[0] != 0) : is_static;
                                    if (clicked && can_preview) {
                                        if (is_animated_bg) {
                                            std::string full_ext = sce_url_ext(item.wallpaper_url);
                                            std::string full_fn = std::string(prefix) + "wallpaper_" + item_label + full_ext;
                                            s_sce_preview_key = folder + "\\" + full_fn;
                                        } else {
                                            s_sce_preview_key = tex_path;
                                        }
                                        // Build nav keys for same type within this series
                                        s_sce_preview_nav_keys.clear();
                                        int nav_slot = 0;
                                        for (int ni : item_indices) {
                                            ++nav_slot;
                                            auto &nitem = items[ni];
                                            char npfx[8]{};
                                            snprintf(npfx, sizeof(npfx), "%02d_", nav_slot);
                                            std::string nlbl = sce_sanitize(nitem.name);
                                            std::string npreview_file;
                                            if (is_animated_bg) {
                                                if (!nitem.wallpaper_url[0]) continue;
                                                std::string next = sce_url_ext(nitem.wallpaper_url);
                                                npreview_file = std::string(npfx) + "wallpaper_" + nlbl + next;
                                            } else {
                                                const char *nsrc = nitem.icon_url[0] ? nitem.icon_url : nitem.static_img_url;
                                                std::string next = sce_url_ext(nsrc);
                                                npreview_file = std::string(npfx) + "icon_" + nlbl + next;
                                            }
                                            s_sce_preview_nav_keys.push_back(folder + "\\" + npreview_file);
                                        }
                                    }

                                    // Name (wrapped to card width)
                                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w);
                                    ImGui::TextUnformatted(item.name);
                                    ImGui::PopTextWrapPos();

                                    // Rarity (coloured)
                                    if (item.rarity[0] && strcmp(item.rarity, "Unknown") != 0)
                                        ImGui::TextColored(sce_rarity_color(item.rarity),
                                            "%s", item.rarity);
                                    else
                                        ImGui::TextDisabled(" ");

                                    // Price
                                    if (item.price_text[0])
                                        ImGui::TextDisabled("%s", item.price_text);
                                    else
                                        ImGui::TextDisabled(" ");

                                    // Badge level / XP (types 3 + 4)
                                    if ((tidx == 3 || tidx == 4) && (item.badge_level > 0 || item.badge_xp > 0)) {
                                        if (item.badge_level > 0 && item.badge_xp > 0)
                                            ImGui::TextDisabled("Lv.%d  %d XP", item.badge_level, item.badge_xp);
                                        else if (item.badge_level > 0)
                                            ImGui::TextDisabled("Lv.%d", item.badge_level);
                                        else
                                            ImGui::TextDisabled("%d XP", item.badge_xp);
                                    }

                                    // Emoticon shortcode (type 5)
                                    if (tidx == 5 && item.emoticon_name[0])
                                        ImGui::TextDisabled(":%s:", item.emoticon_name);

                                    // Steam Points cost
                                    if (item.points_price[0])
                                        ImGui::TextDisabled("Pts: %s", item.points_price);

                                    ImGui::EndGroup();
                                }
                                ImGui::EndTable();
                            }
                            ImGui::Spacing();
                        }

                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                    ImGui::Spacing();
                }
            }
            ImGui::End();

            // ── Full-size asset preview (matching native: dimmed bg, centered, prev/next nav) ──
            if (!s_sce_preview_key.empty()) {
                auto &io2 = ImGui::GetIO();

                // Dim everything behind
                ImGui::GetBackgroundDrawList()->AddRectFilled(
                    ImVec2(0, 0), io2.DisplaySize, IM_COL32(0, 0, 0, 180));

                // Load the preview image immediately (no frame cap)
                const IconTexture *ftex = load_preview_image(s_sce_preview_key);

                float pw = io2.DisplaySize.x * 0.75f;
                float ph = pw * (9.f / 16.f);
                if (ftex && ftex->valid && ftex->w > 0 && ftex->h > 0) {
                    ph = pw * ((float)ftex->h / ftex->w);
                    float max_h = io2.DisplaySize.y * 0.85f;
                    if (ph > max_h) { ph = max_h; pw = ph * ((float)ftex->w / ftex->h); }
                }
                ImVec2 wpos((io2.DisplaySize.x - pw) * 0.5f,
                            (io2.DisplaySize.y - ph) * 0.5f);
                ImGui::SetNextWindowPos(wpos, ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.97f);

                bool preview_open = true;
                if (ImGui::Begin("##sce_preview", &preview_open,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
                    constexpr float BTN_H = 28.f;
                    constexpr float BTN_W = 64.f;
                    constexpr float NAV_PAD = 8.f;
                    float img_avail_h = ImGui::GetContentRegionAvail().y - BTN_H - NAV_PAD * 2.f;

                    if (ftex && ftex->valid) {
                        ImVec2 avail(ImGui::GetContentRegionAvail().x, img_avail_h);
                        float sa = (ftex->h > 0) ? (float)ftex->w / ftex->h : 1.0f;
                        float dw = avail.x, dh = avail.x / sa;
                        if (dh > avail.y) { dh = avail.y; dw = avail.y * sa; }
                        float padx = (avail.x - dw) * 0.5f;
                        float pady = (avail.y - dh) * 0.5f;
                        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + padx,
                                                   ImGui::GetCursorPosY() + pady));
                        ImGui::Image(ImTextureRef(ftex->srv.handle), ImVec2(dw, dh));
                    } else {
                        ImGui::SetCursorPosY(img_avail_h * 0.45f);
                        ImGui::SetCursorPosX((pw - ImGui::CalcTextSize("Loading...").x) * 0.5f);
                        ImGui::TextDisabled("Loading...");
                    }

                    // Navigation buttons
                    int nav_cur = -1;
                    for (int ni = 0; ni < (int)s_sce_preview_nav_keys.size(); ++ni)
                        if (s_sce_preview_nav_keys[ni] == s_sce_preview_key) { nav_cur = ni; break; }

                    auto nav_to = [&](int ni) { s_sce_preview_key = s_sce_preview_nav_keys[ni]; };

                    bool can_prev = nav_cur > 0;
                    bool can_next = nav_cur >= 0 && nav_cur < (int)s_sce_preview_nav_keys.size() - 1;

                    // Arrow / A-D key navigation
                    if (can_prev && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)))
                        nav_to(nav_cur - 1);
                    if (can_next && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)))
                        nav_to(nav_cur + 1);

                    // Button bar pinned to bottom
                    ImVec2 cpos = ImGui::GetWindowPos();
                    ImVec2 csz  = ImGui::GetWindowSize();
                    float by = cpos.y + csz.y - BTN_H - 6.f;

                    ImGui::SetCursorScreenPos(ImVec2(cpos.x + 6.f, by));
                    ImGui::BeginDisabled(!can_prev);
                    if (ImGui::Button("< Prev", ImVec2(BTN_W, BTN_H)) && can_prev)
                        nav_to(nav_cur - 1);
                    ImGui::EndDisabled();

                    // Counter label centred
                    if (nav_cur >= 0) {
                        char cnt[32]{};
                        snprintf(cnt, sizeof(cnt), "%d / %d",
                            nav_cur + 1, (int)s_sce_preview_nav_keys.size());
                        ImVec2 tsz = ImGui::CalcTextSize(cnt);
                        ImGui::SetCursorScreenPos(ImVec2(
                            cpos.x + (csz.x - tsz.x) * 0.5f,
                            by + (BTN_H - tsz.y) * 0.5f));
                        ImGui::TextDisabled("%s", cnt);
                    }

                    ImGui::SetCursorScreenPos(ImVec2(cpos.x + csz.x - BTN_W - 6.f, by));
                    ImGui::BeginDisabled(!can_next);
                    if (ImGui::Button("Next >", ImVec2(BTN_W, BTN_H)) && can_next)
                        nav_to(nav_cur + 1);
                    ImGui::EndDisabled();

                    // Close: Esc, right-click, or clicking outside
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                        (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                          ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                        preview_open = false;
                }
                ImGui::End();
                if (!preview_open)
                    s_sce_preview_key.clear();
            }

            // Free textures when browser is closed
            if (!browser_open) {
                s_show_sce_browser = false;
                s_sce_preview_key.clear();
                s_sce_preview_nav_keys.clear();
                free_sce_textures();
            }
        }
    }
}

/* ── Friends list section ─────────────────────────────────────────────── */

static void render_friends_list()
{
    if (!s_bridge.GetFriendCount || !s_bridge.GetFriends) return;

    int friend_count = s_bridge.GetFriendCount();
    if (friend_count <= 0) return;

    std::vector<GSE_Friend> friends(friend_count > 256 ? 256 : friend_count);
    int count = s_bridge.GetFriends(friends.data(), (int)friends.size());

    // Invite All button (matching native: shown when lobby exists)
    // We check if any friend is joinable as a proxy for having a lobby
    bool has_lobby = false;
    for (int i = 0; i < count && !has_lobby; ++i)
        if (friends[i].is_joinable) has_lobby = true;

    if (has_lobby && s_bridge.InviteAllFriends) {
        if (ImGui::Button("Invite All##PopupInviteAllFriends")) {
            s_bridge.InviteAllFriends();
        }
    }

    // List box with friends (matching native)
    int list_h_items = count < 7 ? count : 7;
    if (list_h_items < 1) list_h_items = 1;
    float item_h = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 list_size(0, item_h * list_h_items + ImGui::GetStyle().FramePadding.y * 2);

    if (ImGui::BeginListBox("##friends_list", list_size)) {
        for (int i = 0; i < count; ++i) {
            auto &f = friends[i];
            ImGui::PushID(i);

            // Online friends in normal text, offline greyed-out (matching native)
            if (!f.is_online)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

            ImGui::Selectable(f.name, false, ImGuiSelectableFlags_AllowDoubleClick);

            if (!f.is_online)
                ImGui::PopStyleColor();

            // Context menu (matching native: Chat, Copy ID, Invite, Join)
            if (ImGui::BeginPopupContextItem("Friends_ContextMenu", 1)) {
                bool close = false;

                // Copy ID (always available)
                if (ImGui::Button("Copy ID")) {
                    close = true;
                    char id_str[32];
                    snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)f.steam_id);
                    ImGui::SetClipboardText(id_str);
                }

                // Invite (if we have a lobby)
                if (has_lobby && s_bridge.FriendAction) {
                    if (ImGui::Button("Invite##PopupInviteToGame")) {
                        close = true;
                        s_bridge.FriendAction(f.steam_id, GSE_FRIEND_ACTION_INVITE);
                    }
                }

                // Join (if friend is joinable)
                if (f.is_joinable && s_bridge.FriendAction) {
                    if (ImGui::Button("Join##PopupAcceptInvite")) {
                        close = true;
                        s_bridge.FriendAction(f.steam_id, GSE_FRIEND_ACTION_JOIN);
                    }
                }

                if (close)
                    ImGui::CloseCurrentPopup();

                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

/* ── Achievement list (separate window, matching native layout exactly) ── */

static bool s_ach_sort_schema_order = true;

static void render_achievement_list()
{
    if (!s_bridge.GetAchievements || !s_bridge.GetAchievementCount) return;

    int total = s_bridge.GetAchievementCount();
    if (total <= 0) return;

    int fetch = total > 2048 ? 2048 : total;
    std::vector<GSE_Achievement> achs(fetch);
    int count = s_bridge.GetAchievements(achs.data(), fetch);
    if (count <= 0) return;

    // Count achieved
    int achieved_count = 0;
    for (int i = 0; i < count; ++i)
        if (achs[i].achieved) achieved_count++;

    auto &io = ImGui::GetIO();
    float font_size = ImGui::GetFontSize();
    float noti_w = io.DisplaySize.x * NOTIF_WIDTH_FRAC;
    float min_w = font_size * 32.0f > noti_w ? font_size * 32.0f : noti_w;

    // Separate window with size constraints (matching native)
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, font_size * 32.0f), ImVec2(8192, 8192));
    ImGui::SetNextWindowBgAlpha(1.0f);

    if (!ImGui::Begin("Achievements##gse_ach", &s_show_achievements)) {
        ImGui::End();
        return;
    }

    // ── Completion progress bar with shadow text overlay (matching native exactly) ──
    {
        float fill = count > 0 ? (float)achieved_count / count : 0.0f;
        float pct  = fill * 100.0f;

        char left_buf[32]{};
        snprintf(left_buf, sizeof(left_buf), "%d/%d", achieved_count, count);
        char right_buf[32]{};
        snprintf(right_buf, sizeof(right_buf), "%.1f%%", pct);

        const float bar_h = font_size + ImGui::GetStyle().FramePadding.y * 2.0f;
        ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        float bar_width = ImGui::GetContentRegionAvail().x;
        ImGui::ProgressBar(fill, ImVec2(-1.0f, bar_h), "");

        auto *dl = ImGui::GetWindowDrawList();
        constexpr ImU32 shadow_col = IM_COL32(0, 0, 0, 200);
        constexpr ImU32 text_col   = IM_COL32(255, 255, 255, 255);
        auto draw_sh = [&](ImVec2 pos, const char *text) {
            dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow_col, text);
            dl->AddText(pos, text_col, text);
        };

        // Left: x/y
        ImVec2 left_sz = ImGui::CalcTextSize(left_buf);
        ImVec2 left_pos = { bar_pos.x + 4.0f, bar_pos.y + (bar_h - left_sz.y) * 0.5f };
        draw_sh(left_pos, left_buf);

        // Right: percentage
        ImVec2 right_sz = ImGui::CalcTextSize(right_buf);
        ImVec2 right_pos = { bar_pos.x + bar_width - right_sz.x - 4.0f, bar_pos.y + (bar_h - right_sz.y) * 0.5f };
        draw_sh(right_pos, right_buf);
    }

    // ── Reset / Simulate buttons (matching native) ──
    if (s_bridge.ResetAchievements) {
        if (ImGui::Button("Reset##ach_reset"))
            s_bridge.ResetAchievements();
        ImGui::SameLine();
    }
    if (s_bridge.SimulateAchievements) {
        if (ImGui::Button("Simulate##ach_simulate"))
            s_bridge.SimulateAchievements();
    }

    // ── Sort control (matching native toggle) ──
    ImGui::BeginChild("Achievements##ach_child");

    if (ImGui::Button(s_ach_sort_schema_order ? "Sort: Schema Order##ach_srt" : "Sort: Global %##ach_srt"))
        s_ach_sort_schema_order = !s_ach_sort_schema_order;

    ImGui::Separator();

    // ── Comparator (matching native: unlocked-recent first, then locked, then hidden) ──
    auto ach_compare = [&](int ai, int bi) -> bool {
        const auto &a = achs[ai];
        const auto &b = achs[bi];
        bool a_hidden = a.hidden && !a.achieved;
        bool b_hidden = b.hidden && !b.achieved;
        if (a_hidden != b_hidden) return !a_hidden;
        if (a_hidden) return false;
        if (a.achieved != b.achieved) return a.achieved > b.achieved;
        if (a.achieved) return a.unlock_time > b.unlock_time;
        if (!s_ach_sort_schema_order) {
            float pa = a.global_percent;
            float pb = b.global_percent;
            if (pa < 0) pa = -1.0f;
            if (pb < 0) pb = -1.0f;
            if (pa != pb) return pa > pb;
        }
        return ai < bi; // schema order tie-break
    };

    // Sort indices
    std::vector<int> sorted_idx(count);
    for (int i = 0; i < count; ++i) sorted_idx[i] = i;
    std::stable_sort(sorted_idx.begin(), sorted_idx.end(), ach_compare);

    // ── Render each achievement (matching native render_ach_item lambda) ──
    const float bar_h = font_size;
    const float icon_col_w = ICON_SIZE;

    for (int si = 0; si < count; ++si) {
        auto &a = achs[sorted_idx[si]];
        bool achieved = a.achieved != 0;
        bool hidden = a.hidden && !achieved;

        ImGui::PushID(sorted_idx[si]);
        ImGui::Separator();

        // Title
        if (hidden) ImGui::Text("[Hidden Achievement]");
        else        ImGui::Text("%s", a.title);

        // Icon + description table (matching native 2-column layout)
        if (ImGui::BeginTable("##ach_tbl", 2)) {
            ImGui::TableSetupColumn("img", ImGuiTableColumnFlags_WidthFixed, icon_col_w);
            ImGui::TableSetupColumn("txt");
            ImGui::TableNextRow(ImGuiTableRowFlags_None, icon_col_w);

            ImGui::TableSetColumnIndex(0);
            {
                // Pick the appropriate icon variant: color for achieved, gray for locked
                const uint8_t *px = achieved ? a.icon_pixels : a.icon_gray_pixels;
                int pw = achieved ? a.icon_w : a.icon_gray_w;
                int ph = achieved ? a.icon_h : a.icon_gray_h;
                // Fall back to color icon if gray variant is not available
                if (!px && !achieved && a.icon_pixels) {
                    px = a.icon_pixels; pw = a.icon_w; ph = a.icon_h;
                }

                char icon_key[320];
                snprintf(icon_key, sizeof(icon_key), "ach_%s_%d", a.name, achieved ? 1 : 0);
                const IconTexture *icon = get_or_upload_icon(icon_key, px, pw, ph);
                if (icon) {
                    ImGui::Image(ImTextureRef(icon->srv.handle), ImVec2(icon_col_w, icon_col_w));
                } else {
                    // Fallback: colored placeholder
                    ImVec4 icon_col = achieved
                        ? ImVec4(0.2f, 0.7f, 0.2f, 1.0f)
                        : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, icon_col);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, icon_col);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, icon_col);
                    ImGui::Button(achieved ? u8"\u2713" : (hidden ? "?" : u8"\u2717"),
                        ImVec2(icon_col_w, icon_col_w));
                    ImGui::PopStyleColor(3);
                }
            }

            ImGui::TableSetColumnIndex(1);
            if (!hidden) ImGui::TextWrapped("%s", a.description);
            else         ImGui::TextDisabled("%s", a.description);

            ImGui::EndTable();
        }

        // ── Status bar (ProgressBar + shadow text overlay, matching native exactly) ──
        {
            const char *sym;
            ImU32 sym_col;
            bool has_progress = !achieved && a.max_progress > 0;
            if (achieved) {
                sym = u8"\u2713"; sym_col = COL_ACH_ACHIEVED;
            } else if (has_progress && a.progress > 0) {
                sym = u8"\u25B6"; sym_col = COL_ACH_PROGRESS;
            } else {
                sym = u8"\u2717"; sym_col = COL_ACH_LOCKED;
            }

            char date_buf[128]{};
            if (achieved && a.unlock_time > 0) {
                time_t t = (time_t)a.unlock_time;
                struct tm tm_buf;
                localtime_s(&tm_buf, &t);
                strftime(date_buf, sizeof(date_buf), "%Y/%m/%d - %H:%M:%S", &tm_buf);
            }

            char pbuf[32]{};
            if (has_progress) snprintf(pbuf, sizeof(pbuf), "%u/%u", a.progress, a.max_progress);

            float fill = achieved ? 1.0f : (has_progress ? (float)a.progress / (float)a.max_progress : 0.0f);
            ImVec2 sbar_pos = ImGui::GetCursorScreenPos();
            float sbar_width = ImGui::GetContentRegionAvail().x;
            ImGui::ProgressBar(fill, ImVec2(-1.0f, bar_h), "");

            auto *dl = ImGui::GetWindowDrawList();
            ImFont *fnt = ImGui::GetFont();
            const float sym_font_sz = bar_h * 0.8f;
            constexpr ImU32 shadow_col = IM_COL32(0, 0, 0, 200);

            auto draw_shadowed = [&](ImVec2 pos, ImU32 col, const char *text) {
                dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow_col, text);
                dl->AddText(pos, col, text);
            };
            auto draw_shadowed_ex = [&](ImFont *f, float sz, ImVec2 pos, ImU32 col, const char *text) {
                dl->AddText(f, sz, ImVec2(pos.x + 1, pos.y + 1), shadow_col, text);
                dl->AddText(f, sz, pos, col, text);
            };

            // Estimate symbol size using current font metrics scaled to sym_font_sz
            ImVec2 cur_sz = ImGui::CalcTextSize(sym);
            float scale = sym_font_sz / ImGui::GetFontSize();
            ImVec2 sym_sz = { cur_sz.x * scale, cur_sz.y * scale };
            ImVec2 sym_pos = { sbar_pos.x + 4.0f, sbar_pos.y + (bar_h - sym_sz.y) * 0.5f };
            draw_shadowed_ex(fnt, sym_font_sz, sym_pos, sym_col, sym);

            if (achieved && date_buf[0]) {
                float date_x = sym_pos.x + sym_sz.x + 4.0f;
                ImVec2 date_sz = ImGui::CalcTextSize(date_buf);
                ImVec2 date_pos = { date_x, sbar_pos.y + (bar_h - date_sz.y) * 0.5f };
                draw_shadowed(date_pos, IM_COL32(255, 255, 255, 255), date_buf);
            }

            if (has_progress) {
                ImVec2 pbar_sz = ImGui::CalcTextSize(pbuf);
                ImVec2 pbar_pos = { sbar_pos.x + (sbar_width - pbar_sz.x) * 0.5f, sbar_pos.y + (bar_h - pbar_sz.y) * 0.5f };
                draw_shadowed(pbar_pos, IM_COL32(255, 255, 255, 255), pbuf);
            }
        }

        // ── Global % ──
        if (a.global_percent >= 0.0f)
            ImGui::TextDisabled("%.1f%% of players", a.global_percent);

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
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
