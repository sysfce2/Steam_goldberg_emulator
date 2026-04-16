#ifdef EMU_OVERLAY

// if you're wondering about text like: ##PopupAcceptInvite
// these are unique labels (keys) for each button/label/text,etc...
// ImGui uses the labels as keys, adding a suffic like "My Text##SomeKey"
// avoids confusing ImGui when another label has the same text "MyText"

#include "overlay/steam_overlay.h"

#include <thread>
#include <string>
#include <sstream>
#include <cctype>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <random>
#include <ctime>
#include <numeric>

#include "InGameOverlay/RendererDetector.h"

#include "dll/dll.h"
#include "dll/settings_parser.h"
#include "dll/steam_app_ids.h"

// translation
#include "overlay/steam_overlay_translations.h"
// fonts
#include "fonts/unifont.hpp"
// builtin audio
#include "overlay/notification.h"
#include "overlay_bridge.h"

// forward declaration — defined later in this file
static void format_ip_address(uint32 ip, char *buf, size_t len);

#define URL_WINDOW_NAME "URL Window"

// Fallback in case the build system did not inject EMU_BUILD_STRING / EMU_BUILD_DATE_STRING.
#ifndef EMU_BUILD_STRING
  #define EMU_BUILD_STRING "unknown"
#endif
#ifndef EMU_BUILD_DATE_STRING
  #define EMU_BUILD_DATE_STRING "unknown"
#endif

// Swapchain colour-space classification for overlay gamma correction.
//
// The overlay renders into the game's back-buffer.  Depending on the swap chain
// format the GPU interprets pixel values differently, so we must pre-transform
// sRGB image bytes and ImGui colours to match.
//
// Classification:
//   SCS_UNKNOWN       (-1)  Not yet detected — treated as SDR (no transform).
//   SCS_LINEAR_HDR    ( 0)  FP16 scRGB / FP32 / 16-bit linear UNORM.
//                            Needs:  sRGB→linear decode  +  SDR-white scale.
//   SCS_SDR_UNORM     ( 1)  Standard 8- or 16-bit SDR (no hw sRGB encoding).
//                            Needs:  optional mild contrast boost.
//   SCS_HDR10_PQ      ( 2)  10-bit HDR10 with PQ (ST.2084) transfer.
//                            Needs:  sRGB→linear→PQ encode  +  nit scaling.
//   SCS_SDR_SRGB_RTV  ( 3)  _SRGB back-buffer (hw applies sRGB on RTV write).
//                            Needs:  sRGB→linear decode (NO SDR-white scale)
//                            so the hardware re-encodes it back correctly.
enum SwapchainColorSpace : int {
    SCS_UNKNOWN      = -1,
    SCS_LINEAR_HDR   =  0,   // FP16 scRGB, FP32, 16-bit linear UNORM
    SCS_SDR_UNORM    =  1,   // Standard SDR 8/16-bit
    SCS_HDR10_PQ     =  2,   // HDR10 PQ (ST.2084, R10G10B10A2)
    SCS_SDR_SRGB_RTV =  3,   // _SRGB back-buffer (hw sRGB encoding)
};
static SwapchainColorSpace s_swapchain_cs = SCS_UNKNOWN;
// File-scope pointer to the current Overlay_Appearance, set once per frame in overlay_render_proc.
static const Overlay_Appearance *s_ov_app = nullptr;

// Resolve the effective colour space, respecting the user's Swapchain_Override setting.
static SwapchainColorSpace effective_swapchain_cs()
{
    if (s_ov_app) {
        using SO = Overlay_Appearance::SwapchainOverride;
        switch (s_ov_app->swapchain_override) {
            case SO::LinearHDR: return SCS_LINEAR_HDR;
            case SO::HDR10PQ:   return SCS_HDR10_PQ;
            case SO::SrgbRTV:   return SCS_SDR_SRGB_RTV;
            case SO::SDR:       return SCS_SDR_UNORM;
            default: break; // Auto — fall through to detected
        }
    }
    return s_swapchain_cs;
}
static const char* s_swapchain_fmt_str   = "Detecting..."; // DXGI format name
static const char* s_swapchain_type_str  = "Detecting..."; // HDR/SDR type  |  gamma  |  colour gamut
// SDR white level scale for HDR colour correction.
//   s_sdr_white_scale = sdr_white_nits / 80.0f
//   1.0f = standard 80-nit SDR white (default / fallback)
//   e.g. 200 nits SDR white -> scale = 2.5 (overlay and ImGui colours boosted proportionally)
static float       s_sdr_white_scale     = 1.0f;
static bool        s_sdr_scale_queried   = false; // true once display info has been queried
static bool        s_pending_sdr_refresh = false; // set on Reset/Removing; drained in overlay_render_proc

// Forward declaration — defined after query_display_hdr_details() below.
struct DisplayHdrDetail_t;
static std::vector<DisplayHdrDetail_t> refresh_sdr_white_scale();
static ImVec4 adjust_imgui_color_for_swapchain(ImVec4 c);

// ---------------------------------------------------------------------------
// Heuristic: determine whether 10-bit R10G10B10A2 / A2R10G10B10 / A2B10G10R10
// pixel data is PQ (HDR10, ST.2084) encoded or plain SDR (sRGB / gamma 2.2).
//
// R10G10B10A2_UNORM is an ambiguous format — it can carry either PQ-encoded
// HDR10 content (game manages HDR, sets DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
// or ordinary sRGB/gamma SDR content in 10-bit precision (display-managed HDR,
// where the DWM applies SDR→HDR conversion after compositing).
//
// Without the DXGI colour-space flag (the InGameOverlay callback only reports
// pixel format), we sample the actual back-buffer pixels:
//
//   PQ reference levels (10-bit code values out of 1023):
//       80 nit (SDR white) → PQ ≈ 520        400 nit → PQ ≈ 681
//     1000 nit             → PQ ≈ 783       4000 nit → PQ ≈ 945
//
//   SDR levels: full white = 1023.  Bright UI and surfaces routinely exceed 900.
//
// If a significant fraction of sampled pixels have any colour channel above 896
// (≈ PQ 2500 nit — extremely rare even in bright HDR scenes), the content is
// almost certainly SDR.
// ---------------------------------------------------------------------------
static bool detect_10bit_content_is_pq(const InGameOverlay::ScreenshotCallbackParameter_t* sc)
{
    if (!sc || !sc->Data || sc->Width == 0 || sc->Height == 0)
        return true;  // unable to determine — fall back to PQ (legacy default)

    const uint8_t* base   = static_cast<const uint8_t*>(sc->Data);
    const uint32_t stride = sc->Pitch;                                // bytes per row
    const uint32_t step_x = (std::max)(sc->Width  / 32u, 1u);        // ~32 samples across
    const uint32_t step_y = (std::max)(sc->Height / 32u, 1u);        // ~32 samples down
    constexpr uint32_t kThreshold = 896;  // ~87.6 % of 1023 → PQ ≈ 2500 nit

    uint32_t above = 0;
    uint32_t total = 0;

    for (uint32_t y = 0; y < sc->Height; y += step_y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(base + (size_t)y * stride);
        for (uint32_t x = 0; x < sc->Width; x += step_x) {
            uint32_t px  = row[x];
            uint32_t ch0 =  px        & 0x3FFu;
            uint32_t ch1 = (px >> 10) & 0x3FFu;
            uint32_t ch2 = (px >> 20) & 0x3FFu;
            uint32_t mx  = (ch0 > ch1) ? ch0 : ch1;
            if (ch2 > mx) mx = ch2;
            if (mx > kThreshold) ++above;
            ++total;
        }
    }

    // If more than 5 % of sampled pixels have any channel above 896, classify
    // as SDR.  In real PQ content, >2500 nit pixels are <1 % of the image.
    return total > 0 && (above * 100u / total) < 5u;
}

// Used in both OverlayHookReady and the [Refresh] button to (re-)arm one-shot format detection.
static void arm_swapchain_format_detect(InGameOverlay::RendererHook_t* r)
{
    s_swapchain_cs = SCS_UNKNOWN;
    s_swapchain_fmt_str   = "Detecting...";
    s_swapchain_type_str  = "Detecting...";
    r->SetScreenshotCallback([](InGameOverlay::ScreenshotCallbackParameter_t const* sc, void* user) {
        using F = InGameOverlay::ScreenshotDataFormat_t;
        if (!sc || sc->Format == F::Unknown) {
            s_swapchain_fmt_str  = "Unknown";
            s_swapchain_type_str = "Unknown";
            s_swapchain_cs = SCS_SDR_UNORM;
        } else {
            switch (sc->Format) {
                // ---- HDR / linear float formats ------------------------------------
                case F::R16G16B16A16_FLOAT:
                    s_swapchain_fmt_str  = "R16G16B16A16_FLOAT";
                    s_swapchain_type_str = "HDR  |  scRGB / Linear  |  BT.709+";
                    s_swapchain_cs = SCS_LINEAR_HDR;
                    break;
                case F::R16G16B16A16_UNORM:
                    s_swapchain_fmt_str  = "R16G16B16A16_UNORM";
                    s_swapchain_type_str = "HDR  |  Linear UNORM  |  BT.2020";
                    s_swapchain_cs = SCS_LINEAR_HDR;
                    break;
                case F::R32G32B32A32_FLOAT:
                    s_swapchain_fmt_str  = "R32G32B32A32_FLOAT";
                    s_swapchain_type_str = "HDR  |  Linear FP32  |  wide gamut";
                    s_swapchain_cs = SCS_LINEAR_HDR;
                    break;
                // ---- 10-bit formats (PQ or SDR — determined by pixel heuristic) ------
                case F::R10G10B10A2:
                    s_swapchain_fmt_str  = "R10G10B10A2_UNORM";
                    if (detect_10bit_content_is_pq(sc)) {
                        s_swapchain_type_str = "HDR10  |  PQ (ST.2084)  |  BT.2020";
                        s_swapchain_cs = SCS_HDR10_PQ;
                    } else {
                        s_swapchain_type_str = "SDR  |  sRGB  |  10-bit (display-managed HDR)";
                        s_swapchain_cs = SCS_SDR_UNORM;
                    }
                    break;
                case F::A2R10G10B10:
                    s_swapchain_fmt_str  = "A2R10G10B10_UNORM";
                    if (detect_10bit_content_is_pq(sc)) {
                        s_swapchain_type_str = "HDR10  |  PQ (ST.2084)  |  BT.2020";
                        s_swapchain_cs = SCS_HDR10_PQ;
                    } else {
                        s_swapchain_type_str = "SDR  |  sRGB  |  10-bit (display-managed HDR)";
                        s_swapchain_cs = SCS_SDR_UNORM;
                    }
                    break;
                case F::A2B10G10R10:
                    s_swapchain_fmt_str  = "A2B10G10R10_UNORM";
                    if (detect_10bit_content_is_pq(sc)) {
                        s_swapchain_type_str = "HDR10  |  PQ (ST.2084)  |  BT.2020";
                        s_swapchain_cs = SCS_HDR10_PQ;
                    } else {
                        s_swapchain_type_str = "SDR  |  sRGB  |  10-bit (display-managed HDR)";
                        s_swapchain_cs = SCS_SDR_UNORM;
                    }
                    break;
                // ---- 8-bit SDR formats (_SRGB = hardware sRGB encoding on RTV) -----
                case F::R8G8B8A8_SRGB:
                    s_swapchain_fmt_str  = "R8G8B8A8_UNORM_SRGB";
                    s_swapchain_type_str = "SDR  |  sRGB (hw decode)  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_SRGB_RTV;
                    break;
                case F::B8G8R8A8_SRGB:
                    s_swapchain_fmt_str  = "B8G8R8A8_UNORM_SRGB";
                    s_swapchain_type_str = "SDR  |  sRGB (hw decode)  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_SRGB_RTV;
                    break;
                case F::B8G8R8X8_SRGB:
                    s_swapchain_fmt_str  = "B8G8R8X8_UNORM_SRGB";
                    s_swapchain_type_str = "SDR  |  sRGB (hw decode)  |  Rec.709  (no alpha)";
                    s_swapchain_cs = SCS_SDR_SRGB_RTV;
                    break;
                case F::R8G8B8A8:
                    s_swapchain_fmt_str  = "R8G8B8A8_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B8G8R8A8:
                    s_swapchain_fmt_str  = "B8G8R8A8_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B8G8R8X8:
                    s_swapchain_fmt_str  = "B8G8R8X8_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709  (no alpha)";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::R8G8B8:
                    s_swapchain_fmt_str  = "R8G8B8";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709  (24-bit packed)";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::X8R8G8B8:
                    s_swapchain_fmt_str  = "X8R8G8B8";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::A8R8G8B8:
                    s_swapchain_fmt_str  = "A8R8G8B8";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                // ---- 16-bit SDR formats (legacy/DX9) -------------------------------
                case F::R5G6B5:
                    s_swapchain_fmt_str  = "R5G6B5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit RGB565";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B5G6R5:
                    s_swapchain_fmt_str  = "B5G6R5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit BGR565";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::X1R5G5B5:
                    s_swapchain_fmt_str  = "X1R5G5B5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit 1555";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::A1R5G5B5:
                    s_swapchain_fmt_str  = "A1R5G5B5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit 1555";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B5G5R5A1:
                    s_swapchain_fmt_str  = "B5G5R5A1_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit 5551";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                default:
                    s_swapchain_fmt_str  = "Unknown format";
                    s_swapchain_type_str = "Unknown";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
            }
        }
        auto* r2 = static_cast<InGameOverlay::RendererHook_t*>(user);
        r2->SetScreenshotCallback(nullptr, nullptr);
        r2->TakeScreenshot(InGameOverlay::ScreenshotType_t::None);
    }, r);
    r->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
}

static constexpr int max_window_id = 10000;
static constexpr int base_notif_window_id  = 0 * max_window_id;
static constexpr int base_friend_window_id = 1 * max_window_id;
static constexpr int base_friend_item_id   = 2 * max_window_id;

// look for the column 'API language code' here: https://partner.steamgames.com/doc/store/localization/languages
static constexpr const char* valid_languages[] = {
    "english",
    "arabic",
    "bulgarian",
    "schinese",
    "tchinese",
    "czech",
    "danish",
    "dutch",
    "finnish",
    "french",
    "german",
    "greek",
    "hungarian",
    "italian",
    "japanese",
    "koreana",
    "norwegian",
    "polish",
    "portuguese",
    "brazilian",
    "romanian",
    "russian",
    "spanish",
    "latam",
    "swedish",
    "thai",
    "turkish",
    "ukrainian",
    "vietnamese",
    "croatian",
    "indonesian",
};


// ListBoxHeader() is deprecated and inlined inside <imgui.h>
// Helper to calculate size from items_count and height_in_items
static inline bool ImGuiHelper_BeginListBox(const char* label, int items_count) {
    int min_items = items_count < 7 ? items_count : 7;
    float height = ImGui::GetTextLineHeightWithSpacing() * (min_items + 0.25f) + ImGui::GetStyle().FramePadding.y * 2.0f;
    return ImGui::BeginListBox(label, ImVec2(0.0f, height));
}


void Steam_Overlay::overlay_run_callback(void* object)
{
    // PRINT_DEBUG_ENTRY();
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->steam_run_callback();
}

void Steam_Overlay::overlay_networking_callback(void* object, Common_Message* msg)
{
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->networking_msg_received(msg);
}

void Steam_Overlay::parse_key_combo()
{
    static const std::unordered_map<InGameOverlay::ToggleKey, std::string_view> KEYS_MAP {
        { InGameOverlay::ToggleKey::SHIFT, "shift" },
        { InGameOverlay::ToggleKey::CTRL,  "ctrl"  },
        { InGameOverlay::ToggleKey::ALT,   "alt"   },
        { InGameOverlay::ToggleKey::TAB,   "tab"   },
        { InGameOverlay::ToggleKey::F1,    "fn1"   },
        { InGameOverlay::ToggleKey::F2,    "fn2"   },
        { InGameOverlay::ToggleKey::F3,    "fn3"   },
        { InGameOverlay::ToggleKey::F4,    "fn4"   },
        { InGameOverlay::ToggleKey::F5,    "fn5"   },
        { InGameOverlay::ToggleKey::F6,    "fn6"   },
        { InGameOverlay::ToggleKey::F7,    "fn7"   },
        { InGameOverlay::ToggleKey::F8,    "fn8"   },
        { InGameOverlay::ToggleKey::F9,    "fn9"   },
        { InGameOverlay::ToggleKey::F10,   "fn10"  },
        { InGameOverlay::ToggleKey::F11,   "fn11"  },
        { InGameOverlay::ToggleKey::F12,   "fn12"  },
    };

    std::unordered_set<InGameOverlay::ToggleKey> keys_combo{};
    bool use_default = false;
    if (settings->overlay_toggle_keys.empty()) {
        use_default = true;
    } else {
        for (const auto &key_name : settings->overlay_toggle_keys) {
            auto key_it = std::find_if(KEYS_MAP.cbegin(), KEYS_MAP.cend(), [&key_name](decltype(*KEYS_MAP.cbegin()) const &item) {
                return common_helpers::str_cmp_insensitive(item.second, key_name);
            });
            if (KEYS_MAP.cend() != key_it) {
                keys_combo.insert(key_it->first);
            } else {
                use_default = true;
                PRINT_DEBUG("[X] Unknown key '%s', using default key combo Shift + Tab", key_name.c_str());
                break;
            }
        }
    }

    if (use_default) {
        toggle_keys = {
            InGameOverlay::ToggleKey::SHIFT, InGameOverlay::ToggleKey::TAB
        };
    } else {
        toggle_keys = std::vector<InGameOverlay::ToggleKey>(keys_combo.begin(), keys_combo.end());
    }
}

Steam_Overlay::Steam_Overlay(Settings* settings, Local_Storage *local_storage, SteamCallResults* callback_results, SteamCallBacks* callbacks, RunEveryRunCB* run_every_runcb, Networking* network) :
    settings(settings),
    local_storage(local_storage),
    callback_results(callback_results),
    callbacks(callbacks),
    run_every_runcb(run_every_runcb),
    network(network),
    stats(Steam_Overlay_Stats(settings))
{
    // don't even bother initializing the overlay
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    // Only create renderer worker threads when the native overlay is enabled
    if (!settings->disable_overlay) {
    renderer_hook_init_thread = common_helpers::KillableWorker(
        [this](void *){ return renderer_hook_proc(); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(renderer_detector_polling_ms),
        [this] { return !setup_overlay_called; }
    );
    
    renderer_detector_delay_thread = common_helpers::KillableWorker(
        [this](void *){
            request_renderer_detector();
            set_renderer_hook_timeout();
            renderer_hook_init_thread.start();
            return true;
        },
        std::chrono::milliseconds(settings->overlay_hook_delay_sec * 1000),
        std::chrono::milliseconds(0),
        [this] { return !setup_overlay_called; }
    );
    } // !disable_overlay

    parse_key_combo();
    strncpy(username_text, settings->get_local_name(), sizeof(username_text));

    // we need these copies to show the warning only once, then disable the flag
    // avoid manipulating settings->xxx
    this->warn_local_save =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_local_save && settings->overlay_warn_local_save;
    this->warn_bad_appid =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_bad_appid && settings->get_local_game_id().AppID() == 0;

    current_language = 0;
    const char *language = settings->get_language();

    show_user_info = settings->overlay_always_show_user_info;

    int i = 0;
    for (auto &lang : valid_languages) {
        if (common_helpers::str_cmp_insensitive(lang, language)) {
            current_language = i;
            break;
        }

        ++i;
    }

    this->network->setCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    this->run_every_runcb->add(&Steam_Overlay::overlay_run_callback, this);
}

Steam_Overlay::~Steam_Overlay()
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    UnSetupOverlay();

    this->network->rmCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    this->run_every_runcb->remove(&Steam_Overlay::overlay_run_callback, this);
}

void Steam_Overlay::request_renderer_detector()
{
    PRINT_DEBUG_ENTRY();
    // request renderer detection
    future_renderer = InGameOverlay::DetectRenderer();
}

void Steam_Overlay::set_renderer_hook_timeout()
{
    renderer_hook_timeout_ctr = settings->overlay_renderer_detector_timeout_sec /*seconds*/ * 1000 /*milli per second*/ / renderer_detector_polling_ms;
}

void Steam_Overlay::cleanup_renderer_hook()
{
    InGameOverlay::StopRendererDetection();
    InGameOverlay::FreeDetector();
}

bool Steam_Overlay::renderer_hook_proc()
{
    if (renderer_hook_timeout_ctr > 0 && future_renderer.wait_for(std::chrono::milliseconds(renderer_detector_polling_ms)) != std::future_status::ready) {
        return false;
    }

    // free detector resources and check for failure
    cleanup_renderer_hook();
    // exit on failure
    bool final_chance = future_renderer.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready;
    // again check for 'setup_overlay_called' to be extra sure that the overlay wasn't deinitialized
    if (!setup_overlay_called || !final_chance || renderer_hook_timeout_ctr <= 0) {
        PRINT_DEBUG("failed to detect renderer, ctr=%i, overlay was set up=%i",
            renderer_hook_timeout_ctr, (int)setup_overlay_called
        );
        return true;
    }

    // do a one time initialization
    // std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    _renderer = future_renderer.get();
    if (!_renderer) { // is this even possible?
        PRINT_DEBUG("renderer hook was null!");
        return true;
    }
    PRINT_DEBUG("got renderer hook %p for '%s'", _renderer, _renderer->GetLibraryName());
    
    // note: make sure to load all relevant strings before creating the font(s), otherwise some glyphs ranges will be missing
    load_achievements_data();
    load_audio();
    create_fonts();
    
    // setup renderer callbacks
    auto overlay_toggle_callback = [this]() { open_overlay_hook(true); };
    _renderer->OverlayProc = [this]() { overlay_render_proc(); };
    _renderer->OverlayHookReady = [this](InGameOverlay::OverlayHookState state) {
        PRINT_DEBUG("hook state changed to <%i>", (int)state);
        bool is_ready = (state == InGameOverlay::OverlayHookState::Ready || state == InGameOverlay::OverlayHookState::Reset);
        overlay_state_hook(is_ready);

        bool clean_up = (state == InGameOverlay::OverlayHookState::Reset ||
                         state == InGameOverlay::OverlayHookState::Removing);
        if (clean_up) {
            // Defer the display-info query to the next rendered frame so we don't
            // block the render thread (which is inside ResizeBuffers/device-release)
            // with synchronous Windows display-config API calls.
            s_sdr_scale_queried   = false;
            s_pending_sdr_refresh = true;

            // Achievement icon GPU resources are now invalid.
            // The decoded-pixel caches hold pixels processed with the OLD LUT.  Clear them so
            // try_load_ach_icon() re-decodes from raw source data with the updated LUT on next use.
            for (auto &ach : achievements) {
                ach.icon_decoded_data.clear();
                ach.icon_gray_decoded_data.clear();
            }
            // Reset the paginated-upload cursor so re-uploads start from the first achievement.
            last_loaded_ach_icon = 0;

            // SCE asset textures: free the whole cache so they are re-loaded and re-decoded lazily.
            sce_textures_free_all();
        }

        if (is_ready && settings->overlay_appearance.image_gamma == Overlay_Appearance::SrgbDecode::Auto) {
            using RHT = InGameOverlay::RendererHookType_t;
            auto rtype = _renderer->GetRendererHookType();
            bool renderer_needs_decode = (rtype == RHT::DirectX10 || rtype == RHT::DirectX11 ||
                                          rtype == RHT::DirectX12 || rtype == RHT::Vulkan || rtype == RHT::Metal);
            if (renderer_needs_decode) {
                // Cancel any in-flight screenshot first, then re-arm.
                // The Reset fires BEFORE ingame_overlay destroys its render targets;
                // cancelling avoids a stale BeforeOverlay screenshot request crossing the resize.
                _renderer->SetScreenshotCallback(nullptr, nullptr);
                _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::None);
                // One-shot screenshot to detect the actual back-buffer format.
                // BeforeOverlay fires before OverlayProc in the same frame,
                // so s_swapchain_cs is set before any AttachResource call.
                arm_swapchain_format_detect(_renderer);
            }
        }
    };

    bool started = _renderer->StartHook(overlay_toggle_callback, toggle_keys.data(), (int)toggle_keys.size(), &fonts_atlas);
    PRINT_DEBUG("started renderer hook (result=%i)", (int)started);
    
    return true;
}

// note: make sure to load all relevant strings before creating the font(s), otherwise some glyphs ranges will be missing
void Steam_Overlay::create_fonts()
{
    PRINT_DEBUG_ENTRY();

    // disable rounding the texture height to the next power of two
    // see this: https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#4-font-atlas-texture-fails-to-upload-to-gpu
    fonts_atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    float font_size = settings->overlay_appearance.font_size;

    font_cfg.FontDataOwnedByAtlas = false; // https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#loading-font-data-from-memory
    font_cfg.PixelSnapH = true;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.SizePixels = font_size;
    // non-latin characters look ugly and squeezed without this horizontal spacing
    font_cfg.GlyphExtraAdvanceX = settings->overlay_appearance.font_glyph_extra_spacing_x;
    // Y-axis spacing removed: ImGui replaced GlyphExtraSpacing (ImVec2) with GlyphExtraAdvanceX (float) in 2025

    for (const auto &ach : achievements) {
        font_builder.AddText(ach.title.c_str());
        font_builder.AddText(ach.description.c_str());
    }
    for (int i = 0; i < TRANSLATION_NUMBER_OF_LANGUAGES; i++) {
        font_builder.AddText(translationChat[i]);
        font_builder.AddText(translationCopyId[i]);
        font_builder.AddText(translationTestAchievement[i]);
        font_builder.AddText(translationInvite[i]);
        font_builder.AddText(translationInviteAll[i]);
        font_builder.AddText(translationJoin[i]);
        font_builder.AddText(translationInvitedYouToJoinTheGame[i]);
        font_builder.AddText(translationAccept[i]);
        font_builder.AddText(translationRefuse[i]);
        font_builder.AddText(translationSend[i]);
        font_builder.AddText(translationUserPlaying[i]);
        font_builder.AddText(translationRenderer[i]);
        font_builder.AddText(translationShowAchievements[i]);
        font_builder.AddText(translationSettings[i]);
        font_builder.AddText(translationFriends[i]);
        font_builder.AddText(translationAchievementWindow[i]);
        font_builder.AddText(translationListOfAchievements[i]);
        font_builder.AddText(translationAchievements[i]);
        font_builder.AddText(translationHiddenAchievement[i]);
        font_builder.AddText(translationAchievedOn[i]);
        font_builder.AddText(translationNotAchieved[i]);
        font_builder.AddText(translationGlobalAchievementPercent[i]);
        font_builder.AddText(translationGlobalSettingsWindow[i]);
        font_builder.AddText(translationGlobalSettingsWindowDescription[i]);
        font_builder.AddText(translationUsername[i]);
        font_builder.AddText(translationLanguage[i]);
        font_builder.AddText(translationSelectedLanguage[i]);
        font_builder.AddText(translationRestartTheGameToApply[i]);
        font_builder.AddText(translationSave[i]);
        font_builder.AddText(translationWarning[i]);
        font_builder.AddText(translationWarningDescription_badAppid[i]);
        font_builder.AddText(translationWarningDescription_localSave[i]);
        font_builder.AddText(translationSteamOverlayURL[i]);
        font_builder.AddText(translationClose[i]);
        font_builder.AddText(translationPlaying[i]);
        font_builder.AddText(translationAutoAcceptFriendInvite[i]);
        font_builder.AddText(translationFpsCheckbox[i]);
        font_builder.AddText(translationFpsDisplay[i]);
        font_builder.AddText(translationFrametimeCheckbox[i]);
        font_builder.AddText(translationFrametimeDisplay[i]);
        font_builder.AddText(translationFrametimeUnitDisplay[i]);
        font_builder.AddText(translationPlaytimeCheckbox[i]);
        font_builder.AddText(translationPlaytimeDisplay[i]);
    }
    font_builder.AddRanges(fonts_atlas.GetGlyphRangesDefault());
    font_builder.AddChar((ImWchar)0x2713); // ✓ CHECK MARK
    font_builder.AddChar((ImWchar)0x2717); // ✗ BALLOT X
    font_builder.AddChar((ImWchar)0x25B6); // ▶ BLACK RIGHT-POINTING TRIANGLE (in-progress)

    font_builder.BuildRanges(&ranges);
    font_cfg.GlyphRanges = ranges.Data;

    if (settings->overlay_appearance.font_override.size()) {
        fonts_atlas.AddFontFromFileTTF(settings->overlay_appearance.font_override.c_str(), font_size, &font_cfg);
        font_cfg.MergeMode = true; // merge next fonts into the first one, as if they were all just 1 font file
    }

    // note: base85 compressed arrays caused a compiler heap allocation error, regular compression is more guaranteed
    ImFont *font = fonts_atlas.AddFontFromMemoryCompressedTTF(unifont_compressed_data, unifont_compressed_size, font_size, &font_cfg);
    font_notif = font_default = font;
    stats.font = font;
    
    // With ImGui 1.92+ and ImGuiBackendFlags_RendererHasTextures, the backend
    // builds the font atlas automatically — no need to call Build() manually.
    PRINT_DEBUG("fonts added to atlas (backend will build automatically)");

    reset_LastError();
}

void Steam_Overlay::load_audio()
{
    PRINT_DEBUG_ENTRY();

    for (auto &kv : wav_files) {
        std::string file_path{};
        unsigned int file_size{};

        // try local location first, then try global location
        for (const auto &settings_path : { Local_Storage::get_game_settings_path(), local_storage->get_global_settings_path() }) {
            file_path = settings_path + Steam_Overlay::ACH_SOUNDS_FOLDER + PATH_SEPARATOR + kv.first;
            file_size = file_size_(file_path);
            if (file_size) break;
        }

        kv.second.clear();
        if (file_size) {
            kv.second.assign(file_size + 1, 0); // +1 because this will be treated as a null-terminated string later
            int read = Local_Storage::get_file_data(file_path, (char *)&kv.second[0], file_size);
            if (read <= 0) kv.second.clear();
            PRINT_DEBUG("loaded '%s' (read %i/%u bytes)", file_path.c_str(), read, file_size);
        }
    }
}

void Steam_Overlay::load_achievements_data()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    Steam_User_Stats* steamUserStats = get_steam_client()->steam_user_stats;
    uint32 achievements_num = steamUserStats->GetNumAchievements();
    for (uint32 i = 0; i < achievements_num; ++i) {
        Overlay_Achievement ach{};
        ach.name = steamUserStats->GetAchievementName(i);
        ach.title = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "name");
        ach.description = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "desc");
        
        const char *hidden = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "hidden");
        ach.hidden = hidden && hidden[0] == '1';

        bool achieved = false;
        uint32 unlock_time = 0;
        if (steamUserStats->GetAchievementAndUnlockTime(ach.name.c_str(), &achieved, &unlock_time)) {
            ach.achieved = achieved;
            ach.unlock_time = unlock_time;
        } else {
            ach.achieved = false;
            ach.unlock_time = 0;
        }

        float pnMinProgress = 0, pnMaxProgress = 0;
        if (steamUserStats->GetAchievementProgressLimits(ach.name.c_str(), &pnMinProgress, &pnMaxProgress)) {
            ach.progress = (uint32)pnMinProgress;
            ach.max_progress = (uint32)pnMaxProgress;
        }

        if (_renderer) {
            if (ach.icon == nullptr) {
                ach.icon = _renderer->CreateResource();
            }
            if (ach.icon_gray == nullptr) {
                ach.icon_gray = _renderer->CreateResource();
            }
        }

        achievements.emplace_back(ach);
        
        if (!setup_overlay_called) return;
    }

    // save a snapshot for the Reset button
    achievements_snapshot.clear();
    for (const auto &a : achievements) {
        achievements_snapshot.push_back({ a.name, a.achieved, a.progress, a.unlock_time });
    }

    PRINT_DEBUG("count=%u, loaded=%zu", achievements_num, achievements.size());

    if (steamUserStats->global_achievement_percentages_populated) {
        // data already available (e.g. game called the API before overlay init)
        if (settings->overlay_achievement_sort_by_global_percent) {
            SortAchievementsByGlobalPercent(steamUserStats->global_achievement_percentages);
        } else {
            ach_global_percentages = steamUserStats->global_achievement_percentages;
        }
        ach_global_percentages_snapshot = ach_global_percentages;
    } else {
        // trigger fetch if the game hasn't done so yet; result arrives via steam_run_callback
        steamUserStats->RequestGlobalAchievementPercentages();
    }

    // Always trigger SteamHunters fetch once per launch (groups + supplemental % fallback)
    steamUserStats->RequestSteamHuntersData();
}

void Steam_Overlay::SortAchievementsByGlobalPercent(const std::map<std::string, float> &percentages)
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    ach_global_percentages = percentages;
    ach_global_percentages_snapshot = percentages;
    // Match Steam's display order:
    //   1. Unlocked achievements, sorted by unlock time descending (most recent first)
    //   2. Locked achievements, sorted by global percentage descending (easiest first)
    std::stable_sort(achievements.begin(), achievements.end(),
        [&percentages](const Overlay_Achievement &a, const Overlay_Achievement &b) {
            if (a.achieved != b.achieved) return a.achieved > b.achieved; // unlocked before locked
            if (a.achieved) return a.unlock_time > b.unlock_time;          // unlocked: most recent first
            // both locked: higher global % first
            auto ita = percentages.find(a.name);
            auto itb = percentages.find(b.name);
            float pa = (ita != percentages.end()) ? ita->second : -1.0f;
            float pb = (itb != percentages.end()) ? itb->second : -1.0f;
            return pa > pb;
        }
    );
    PRINT_DEBUG("achievements re-sorted (unlocked by recency, locked by global %)");
}

// Called from Steam_User_Stats background thread once SteamHunters data has arrived.
// Picks up any newly populated global percentages (in case Steam API hadn't returned yet).
void Steam_Overlay::UpdateSteamHuntersData()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats->global_achievement_percentages_populated) return;

    // If we don't have percentages yet in the overlay, grab them now
    if (ach_global_percentages.empty()) {
        if (settings->overlay_achievement_sort_by_global_percent) {
            SortAchievementsByGlobalPercent(steamUserStats->global_achievement_percentages);
        } else {
            ach_global_percentages = steamUserStats->global_achievement_percentages;
            ach_global_percentages_snapshot = ach_global_percentages;
        }
        PRINT_DEBUG("UpdateSteamHuntersData: overlay percentages updated from SteamHunters fallback");
    }
}

void Steam_Overlay::NotifySceAssetsReady(uint32_t downloaded, uint32_t skipped, uint32_t total)
{
    PRINT_DEBUG("SceAssets: downloaded=%u skipped=%u total=%u", downloaded, skipped, total);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    sce_asset_progress.downloaded           = downloaded;
    sce_asset_progress.skipped              = skipped;
    sce_asset_progress.total                = total;
    sce_asset_progress.pending_notification = true;
    // request a frame render so the notification is shown promptly
    allow_renderer_frame_processing(true);
}

// called initially and when window size is updated
void Steam_Overlay::overlay_state_hook(bool ready)
{
    PRINT_DEBUG("%i", (int)ready);
    
    // NOTE usage of local objects here cause an exception when this is called with false state
    // the reason is that by the time this hook is called, the object may have been already destructed
    // this is why we use global mutex
    // TODO this also doesn't seem right, no idea why it happens though
    // NOTE after initializing the renderer detector on another thread this was solved
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!setup_overlay_called) return;

    is_ready = ready;

    if (ready) {
        // Antichamber may crash here because ImGui Context is null!, no idea why
        bool not_yet = false;
        if (ImGui::GetCurrentContext() && late_init_imgui.compare_exchange_weak(not_yet, true)) {
            PRINT_DEBUG("late init ImGui");

            ImGuiIO &io = ImGui::GetIO();
            // disable loading the default ini file
            io.IniFilename = NULL;

            ImGuiStyle &style = ImGui::GetStyle();
            // Disable round window
            style.WindowRounding = 0.0;
        }
    }
}

// called when the user presses SHIFT + TAB
bool Steam_Overlay::open_overlay_hook(bool toggle)
{
    if (toggle) {
        ShowOverlay(!show_overlay);
    }

    return show_overlay;
}

void Steam_Overlay::allow_renderer_frame_processing(bool state, bool cleaning_up_overlay)
{
    // this is very important internally it calls the necessary fuctions
    // to properly update ImGui window size on the next overlay_render_proc() call

    // In bridge-only mode _renderer is null — nothing to do.
    if (!_renderer) return;

    if (state) {
        auto new_val = ++renderer_frame_processing_requests;
        if (new_val == 1) { // only take an action on first request
            // allow internal frmae processing
            _renderer->HideOverlayInputs(false);
            PRINT_DEBUG("enabled frame processing (count=%u)", new_val);
        }
    } else {
        if (renderer_frame_processing_requests > 0) {
            auto new_val = --renderer_frame_processing_requests;
            if (!new_val || cleaning_up_overlay) { // only take an action when the requests reach 0 or by force
                _renderer->HideOverlayInputs(true);
                PRINT_DEBUG("disabled frame processing (count=%u, force=%i)", new_val, (int)cleaning_up_overlay);
            }
        }
    }
}

void Steam_Overlay::obscure_game_input(bool state) {
    // In bridge-only mode _renderer is null and ImGui context is not ours.
    if (!_renderer) return;

    if (state) {
        auto new_val = ++obscure_cursor_requests;
        if (new_val == 1) { // only take an action on first request
            ImGuiIO &io = ImGui::GetIO();
            // force draw the cursor, otherwise games like Truberbrook will not have an overlay cursor
            io.MouseDrawCursor = state;
            // not necessary, just to be sure
            io.WantCaptureMouse = state;
            // not necessary, just to be sure
            io.WantCaptureKeyboard = state;

            // clip the cursor
            _renderer->HideAppInputs(true);
            PRINT_DEBUG("obscured app input (count=%u)", new_val);
        }
    } else {
        if (obscure_cursor_requests > 0) {
            auto new_val = --obscure_cursor_requests;
            if (!new_val) { // only take an action when the requests reach 0
                ImGuiIO &io = ImGui::GetIO();
                // force draw the cursor, otherwise games like Truberbrook will not have an overlay cursor
                io.MouseDrawCursor = state;
                // not necessary, just to be sure
                io.WantCaptureMouse = state;
                // not necessary, just to be sure
                io.WantCaptureKeyboard = state;
                
                // restore the old cursor
                _renderer->HideAppInputs(false);
                PRINT_DEBUG("restored app input (count=%u)", new_val);
            }
        }
    }
}

void Steam_Overlay::notify_sound_user_invite(friend_window_state& friend_state)
{
    if (settings->disable_overlay_friend_notification) return;

    if (!(friend_state.window_state & window_state_show)) {
        friend_state.window_state |= window_state_need_attention;
#ifdef __WINDOWS__
        auto wav_data = wav_files.find("overlay_friend_notification.wav");
        if (wav_files.end() != wav_data && wav_data->second.size()) {
            PlaySoundA((LPCSTR)&wav_data->second[0], NULL, SND_ASYNC | SND_MEMORY);
        } else {
            PlaySoundA((LPCSTR)notif_invite_wav, NULL, SND_ASYNC | SND_MEMORY);
        }
#endif
    }
}

void Steam_Overlay::notify_sound_user_achievement()
{
    if (settings->disable_overlay_achievement_notification) return;

#ifdef __WINDOWS__
    auto wav_data = wav_files.find("overlay_achievement_notification.wav");
    if (wav_files.end() != wav_data && wav_data->second.size()) {
        PlaySoundA((LPCSTR)&wav_data->second[0], NULL, SND_ASYNC | SND_MEMORY);
    }
#endif
}

void Steam_Overlay::notify_sound_auto_accept_friend_invite()
{
#ifdef __WINDOWS__
    auto wav_data = wav_files.find("overlay_friend_notification.wav");
    if (wav_files.end() != wav_data && wav_data->second.size()) {
        PlaySoundA((LPCSTR)&wav_data->second[0], NULL, SND_ASYNC | SND_MEMORY);
    } else {
        PlaySoundA((LPCSTR)notif_invite_wav, NULL, SND_ASYNC | SND_MEMORY);
    }
#endif
}

int find_free_id(std::vector<int> &ids, int base)
{
    std::sort(ids.begin(), ids.end());

    int id = base;
    for (auto i : ids)
    {
        if (id < i)
            break;
        id = i + 1;
    }

    return id > (base+max_window_id) ? 0 : id;
}

int find_free_friend_id(const std::map<Friend, friend_window_state, Friend_Less> &friend_windows)
{
    std::vector<int> ids{};
    ids.reserve(friend_windows.size());

    std::for_each(friend_windows.begin(), friend_windows.end(), [&ids](std::pair<Friend const, friend_window_state> const& i)
    {
        ids.emplace_back(i.second.id);
    });
    
    return find_free_id(ids, base_friend_window_id);
}

int find_free_notification_id(std::vector<Notification> const& notifications)
{
    std::vector<int> ids{};
    ids.reserve(notifications.size());

    std::for_each(notifications.begin(), notifications.end(), [&ids](Notification const& i)
    {
        ids.emplace_back(i.id);
    });
    

    return find_free_id(ids, base_friend_window_id);
}

bool Steam_Overlay::submit_notification(
    notification_type type,
    const std::string &msg,
    std::pair<const Friend, friend_window_state> *frd,
    Overlay_Achievement *ach)
{
    PRINT_DEBUG("%i", (int)type);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return false;
    
    int id = find_free_notification_id(notifications);
    if (id == 0) {
        PRINT_DEBUG("error no free id to create a notification window");
        return false;
    }

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)type;
    notif.message = msg;
    notif.frd = frd;
    if (frd) notif.source_friend_id = frd->first.id();
    if (ach) notif.ach = *ach;
    
    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
    // uncomment this block to obscure cursor input and steal focus for these specific notifications
    switch (type) {
        // we want to steal focus for these ones
        case notification_type::invite:
            obscure_game_input(true);
        break;

        // not effective
        case notification_type::achievement_progress:
        case notification_type::achievement:
        case notification_type::auto_accept_invite:
        case notification_type::message:
            // nothing
        break;

        case notification_type::lobby_join_request:
            obscure_game_input(true);
        break;

        case notification_type::lobby_join_request_response:
            // non-interactive, no input stealing
        break;

        case notification_type::friend_lobby_available:
            obscure_game_input(true);
        break;

        default:
            PRINT_DEBUG("error unhandled type %i", (int)type);
        break;
    }

    return true;
}

void Steam_Overlay::add_chat_message_notification(std::string const &message, std::pair<const Friend, friend_window_state> *frd)
{
    if (settings->disable_overlay_friend_notification) return;

    PRINT_DEBUG("'%s'", message.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    submit_notification(notification_type::message, message, frd);
}

void Steam_Overlay::poll_lobby_join_requests()
{
    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    const auto &pending = mm->GetPendingLobbyJoinRequests();

    for (const auto &req : pending) {
        auto key = std::make_pair(req.lobby_id.ConvertToUint64(), req.requester_id.ConvertToUint64());
        if (notified_lobby_join_requests.count(key)) continue;

        notified_lobby_join_requests.insert(key);

        const char *name = steamFriends->GetFriendPersonaName(req.requester_id);
        std::string msg = std::string("Auto join request from ") + (name ? name : "Unknown") + "\nLobby: " + std::to_string(req.lobby_id.ConvertToUint64());

        int id = find_free_notification_id(notifications);
        if (id == 0) continue;

        Notification notif{};
        notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
        notif.id = id;
        notif.type = (uint8)notification_type::lobby_join_request;
        notif.message = msg;
        notif.join_request_lobby_id = req.lobby_id.ConvertToUint64();
        notif.join_request_requester_id = req.requester_id.ConvertToUint64();
        notif.source_friend_id = req.requester_id.ConvertToUint64();

        notifications.emplace_back(notif);
        allow_renderer_frame_processing(true);
        obscure_game_input(true);
    }
}

void Steam_Overlay::add_lobby_join_request_response_notification(uint64 lobby_id, const std::string &owner_name, bool accepted, uint64 source_id)
{
    PRINT_DEBUG("lobby=%llu owner=%s accepted=%d", lobby_id, owner_name.c_str(), (int)accepted);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    std::string msg = std::string("Your join request was ") + (accepted ? "accepted" : "denied") + " by " + owner_name + "\nLobby: " + std::to_string(lobby_id);

    int id = find_free_notification_id(notifications);
    if (id == 0) return;

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)notification_type::lobby_join_request_response;
    notif.message = msg;
    notif.source_friend_id = source_id;

    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
}

void Steam_Overlay::add_lobby_kicked_notification(uint64 lobby_id, const std::string &kicker_name, uint64 source_id)
{
    PRINT_DEBUG("lobby=%llu kicker=%s", lobby_id, kicker_name.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    std::string msg = std::string("You were removed from lobby by ") + kicker_name + "\nLobby: " + std::to_string(lobby_id);

    int id = find_free_notification_id(notifications);
    if (id == 0) return;

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)notification_type::lobby_kicked;
    notif.message = msg;
    notif.source_friend_id = source_id;

    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
}

void Steam_Overlay::show_test_achievement()
{
    PRINT_DEBUG_ENTRY();
    Overlay_Achievement ach{};
    ach.title = translationTestAchievement[current_language];
    ach.description = "~~~ " + ach.title + " ~~~";
    ach.achieved = true;

    // random add icon
    if (achievements.size()) {
        size_t rand_idx = common_helpers::rand_number(achievements.size() - 1);
        auto &rand_ach = achievements[rand_idx];
        bool achieved = rand_idx < (achievements.size() / 2);
        // force upload to GPU if the pagination is request-based
        try_load_ach_icon(rand_ach, achieved, settings->paginated_achievements_icons == 0);
        ach.icon = rand_ach.icon;
        ach.icon_gray = rand_ach.icon_gray;
        // Copy decoded pixel data so the bridge addon can access the icon
        ach.icon_handle = rand_ach.icon_handle;
        ach.icon_gray_handle = rand_ach.icon_gray_handle;
        ach.icon_decoded_data = rand_ach.icon_decoded_data;
        ach.icon_gray_decoded_data = rand_ach.icon_gray_decoded_data;
        ach.name = rand_ach.name;
    }

    // randomly add progress
    bool for_progress = false;
    if (common_helpers::rand_number(1000) % 2) {
        for_progress = true;
        uint32 progress = (uint32)(common_helpers::rand_number(500) / 10 + 50); // [50, 100]
        ach.max_progress = 100;
        ach.progress = progress;
        ach.achieved = false;
    }
    
    post_achievement_notification(ach, for_progress);
    // here we always play the sound for testing
    notify_sound_user_achievement();
}

void Steam_Overlay::build_friend_context_menu(Friend const& frd, friend_window_state& state)
{
    if (ImGui::BeginPopupContextItem("Friends_ContextMenu", 1)) {
        // this is set to true if any button was clicked
        // otherwise, after clicking any button, the menu will be persistent
        bool close_popup = false;

        // user clicked on "chat"
        if (ImGui::Button(translationChat[current_language])) {
            close_popup = true;
            state.window_state |= window_state_show;
        }
        // user clicked on "copy id" on a friend
        if (ImGui::Button(translationCopyId[current_language])) {
            close_popup = true;
            auto friend_id_str = std::to_string(frd.id());
            ImGui::SetClipboardText(friend_id_str.c_str());
        }
        // user clicked on "copy lobby id" on a friend (if they're in a lobby)
        if (frd.lobby_id() != 0 && ImGui::Button("Copy Lobby ID##PopupCopyLobbyId")) {
            close_popup = true;
            auto lobby_id_str = std::to_string(frd.lobby_id());
            ImGui::SetClipboardText(lobby_id_str.c_str());
        }
        // Check if friend is already in our lobby
        bool friend_in_my_lobby = false;
        {
            CSteamID my_lob = settings->get_lobby();
            if (my_lob.IsValid()) {
                Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                if (mm) {
                    int mc = mm->GetNumLobbyMembers(my_lob);
                    for (int mi = 0; mi < mc; ++mi) {
                        if (mm->GetLobbyMemberByIndex(my_lob, mi).ConvertToUint64() == frd.id()) {
                            friend_in_my_lobby = true;
                            break;
                        }
                    }
                }
            }
        }
        // If we have the same appid, activate the invite button
        if (settings->get_local_game_id().AppID() == frd.appid()) {
            // user clicked on "invite to game" (hide if friend already in our lobby)
            std::string translationInvite_tmp(translationInvite[current_language]);
            translationInvite_tmp.append("##PopupInviteToGame");
            if (i_have_lobby && !friend_in_my_lobby && ImGui::Button(translationInvite_tmp.c_str())) {
                close_popup = true;
                state.window_state |= window_state_invite;
                has_friend_action.push(frd);
            }
        }
        // user clicked on "join lobby" (same app only, hide if already in same lobby)
        if (settings->get_local_game_id().AppID() == frd.appid() && state.joinable && !friend_in_my_lobby) {
            std::string translationJoin_tmp(translationJoin[current_language]);
            translationJoin_tmp.append("##PopupJoinLobby");
            if (ImGui::Button(translationJoin_tmp.c_str())) {
                close_popup = true;
                if (!invite_all_friends_clicked) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
            }
        }
        // Show Accept Invite if this friend sent us a pending invite we haven't accepted yet
        if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            if (ImGui::Button("Accept Invite##PopupAcceptInvite")) {
                close_popup = true;
                state.window_state |= window_state_join;
                has_friend_action.push(frd);
            }
        }

        if (close_popup || invite_all_friends_clicked) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Steam_Overlay::build_chat_window()
{
    if (!show_chat) return;

    // Collect friends with open chat tabs
    struct ChatTab {
        const Friend *frd;
        friend_window_state *state;
    };
    std::vector<ChatTab> open_tabs;
    for (auto &[frd, state] : friends) {
        if (state.window_state & window_state_show)
            open_tabs.push_back({&frd, &state});
    }

    if (open_tabs.empty()) {
        show_chat = false;
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    float min_w = io.DisplaySize.x * 0.25f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, ImGui::GetFontSize() * 16), ImVec2(8192, 8192));
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);

    if (ImGui::Begin("Chat##gse_chat_tabbed", &show_chat, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##chat_tabs")) {
            for (auto &tab : open_tabs) {
                auto &frd = *tab.frd;
                auto &state = *tab.state;
                bool needs_attn = (state.window_state & window_state_need_attention) != 0;

                if (needs_attn)
                    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.6f, 0.4f, 0.1f, 1.0f));

                bool tab_open = true;
                std::string tab_label = frd.name() + "###chat_tab_" + std::to_string(frd.id());

                if (ImGui::BeginTabItem(tab_label.c_str(), &tab_open)) {
                    // Clear attention when tab is active
                    if (needs_attn) state.window_state &= ~window_state_need_attention;

                    // ---- 64px friend avatar + 3 info lines ----
                    {
                        const float avatar_size = 64.0f;
                        bool has_avatar = try_load_avatar(state, frd.id());
                        if (has_avatar && state.avatar_resource && state.avatar_resource->GetResourceId() != 0) {
                            ImGui::Image(state.avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                        } else {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), IM_COL32(60, 60, 80, 255));
                            ImGui::Dummy(ImVec2(avatar_size, avatar_size));
                        }
                        ImGui::SameLine();

                        ImVec2 text_start = ImGui::GetCursorPos();
                        uint32 local_appid = settings->get_local_game_id().AppID();

                        // Line 1: Friend name (ID: steamid)
                        ImGui::SetCursorPos(text_start);
                        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "%s", frd.name().c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(ID: %llu)", frd.id());

                        // Line 2: Playing AppID
                        ImGui::SetCursorPosX(text_start.x);
                        if (frd.appid() != 0) {
                            auto it = steam_preowned_app_ids.find(frd.appid());
                            std::string app_name = (it != steam_preowned_app_ids.end()) ? it->second : std::to_string(frd.appid());
                            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Playing %s (AppID %u)", app_name.c_str(), frd.appid());
                        } else {
                            ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "Online");
                        }

                        // Line 3: Status
                        ImGui::SetCursorPosX(text_start.x);
                        bool same_app = (local_appid == frd.appid());
                        if (frd.lobby_id() != 0) {
                            bool frd_is_owner = (frd.lobby_owner_name().size() > 0 && frd.lobby_owner_name() == frd.name());
                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s - %llu",
                                frd_is_owner ? "Has Lobby" : "In Lobby", frd.lobby_id());
                        } else if (same_app) {
                            ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "In Game");
                        } else if (frd.appid() != 0) {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "In another game");
                        } else {
                            ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "Online");
                        }
                    }
                    ImGui::Separator();

                    // Invite accept/refuse
                    if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                        bool same_app = (settings->get_local_game_id().AppID() == frd.appid());
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        ImGui::LabelText("##label", translationInvitedYouToJoinTheGame[current_language], frd.name().c_str(), frd.appid());
                        ImGui::PopStyleColor();
                        if (same_app) {
                            ImGui::SameLine();
                            if (ImGui::Button(translationAccept[current_language])) {
                                state.window_state |= window_state_join;
                                this->has_friend_action.push(frd);
                                state.chat_history.append("[INVITE ACCEPTED] You accepted the invite\n");
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(translationRefuse[current_language])) {
                                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                                state.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                            }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(different game)");
                            ImGui::SameLine();
                            if (ImGui::Button(translationRefuse[current_language])) {
                                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                                state.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                            }
                        }
                    }

                    // Chat history
                    float footer_height = ImGui::GetFrameHeightWithSpacing() + 4;
                    ImGui::InputTextMultiline("##chat_history", &state.chat_history[0], state.chat_history.length(),
                        { -1.0f, -footer_height }, ImGuiInputTextFlags_ReadOnly);

                    // Input bar: [____chat line______] [send]
                    float wnd_width = ImGui::GetContentRegionAvail().x;
                    ImGuiStyle &style = ImGui::GetStyle();
                    wnd_width -= ImGui::CalcTextSize(translationSend[current_language]).x + style.FramePadding.x * 2 + style.ItemSpacing.x + 1;

                    uint64_t frd_id = frd.id();
                    ImGui::PushID((const char *)&frd_id, (const char *)&frd_id + sizeof(frd_id));
                    ImGui::PushItemWidth(wnd_width);

                    bool send_chat_msg = false;
                    if (ImGui::InputText("##chat_line", state.chat_input, max_chat_len, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        send_chat_msg = true;
                        ImGui::SetKeyboardFocusHere(-1);
                    }

                    ImGui::PopItemWidth();
                    ImGui::PopID();

                    ImGui::SameLine();
                    if (ImGui::Button(translationSend[current_language])) {
                        send_chat_msg = true;
                    }

                    if (send_chat_msg) {
                        if (!(state.window_state & window_state_send_message)) {
                            has_friend_action.push(frd);
                            state.window_state |= window_state_send_message;
                        }
                    }

                    ImGui::EndTabItem();
                }

                if (needs_attn)
                    ImGui::PopStyleColor();

                // Close tab
                if (!tab_open) {
                    state.window_state &= ~window_state_show;
                }
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}


std::chrono::milliseconds Steam_Overlay::get_notification_duration(notification_type type)
{
    switch (type)
    {
    case notification_type::message:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_chat);
    
    case notification_type::invite:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);
    
    case notification_type::achievement:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_achievement);
    
    case notification_type::achievement_progress:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_progress);
    
    case notification_type::auto_accept_invite:
        return Notification::default_show_time;
    
    case notification_type::lobby_join_request:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);
    
    case notification_type::lobby_join_request_response:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);
    
    case notification_type::lobby_kicked:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);

    case notification_type::lobby_status:
        return Notification::default_show_time;
    }

    PRINT_DEBUG("ERROR unhandled type %i", (int)type);
    return Notification::default_show_time;
}

// set the position of the next notification
void Steam_Overlay::set_next_notification_pos(std::pair<float, float> scrn_size, std::chrono::milliseconds elapsed, std::chrono::milliseconds duration, const Notification &noti, struct NotificationsCoords &coords)
{
    const float scrn_width = scrn_size.first;
    const float scrn_height = scrn_size.second;

    auto &global_style = ImGui::GetStyle();
    const float padding_all_sides = 2 * (global_style.WindowPadding.y + global_style.WindowPadding.x);

    const float min_noti_width = scrn_width * Notification::width_percent;
    const bool is_achievement = ((notification_type)noti.type == notification_type::achievement ||
                                 (notification_type)noti.type == notification_type::achievement_progress);
    const float noti_width = is_achievement ? min_noti_width :
        (noti.last_width > min_noti_width ? noti.last_width : min_noti_width);
    const float msg_height = ImGui::CalcTextSize(
        noti.message.c_str(),
        noti.message.c_str() + noti.message.size(),
        false,
        noti_width - padding_all_sides - global_style.ItemSpacing.x
    ).y;
    float noti_height = msg_height;

    // Extra height for friend avatar + 3-line header (invite, message, join request, response, kicked)
    const float friend_header_height = (noti.source_friend_id != 0)
        ? std::max(48.0f, ImGui::GetTextLineHeight() * 3.0f) + global_style.ItemSpacing.y + 1.0f /* separator */
        : 0.0f;
    
    // get the required position
    Overlay_Appearance::NotificationPosition pos = Overlay_Appearance::default_pos;
    switch ((notification_type)noti.type) {
    case notification_type::achievement_progress:
    case notification_type::achievement: {
        pos = settings->overlay_appearance.ach_earned_pos;

        const auto &ach_h = noti.ach.value();
        const float title_height = ImGui::CalcTextSize(
            ach_h.title.c_str(), nullptr, false,
            noti_width - padding_all_sides
        ).y;
        const float desc_height = ImGui::CalcTextSize(
            ach_h.description.c_str(), nullptr, false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x - settings->overlay_appearance.icon_size
        ).y;
        const float row_height = std::max(desc_height, (float)settings->overlay_appearance.icon_size);
        noti_height = title_height + global_style.ItemSpacing.y + row_height;

        if ((notification_type)noti.type == notification_type::achievement_progress) {
            if (!noti.ach.value().achieved && noti.ach.value().max_progress > 0) {
                noti_height += settings->overlay_appearance.font_size + global_style.WindowPadding.y;
            }
        }
    }
    break;

    // case notification_type::invite: pos = settings->overlay_appearance.invite_pos; break;
    case notification_type::invite: {
        pos = settings->overlay_appearance.invite_pos;
        const float msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::message:
        pos = settings->overlay_appearance.chat_msg_pos;
        noti_height += friend_header_height;
    break;
    case notification_type::lobby_join_request: {
        pos = settings->overlay_appearance.invite_pos;
        const float ljr_msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + ljr_msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::lobby_join_request_response: {
        pos = settings->overlay_appearance.invite_pos;
        const float ljrr_msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + ljrr_msg_height + global_style.WindowPadding.y;
    }
    break;
    case notification_type::lobby_kicked: {
        pos = settings->overlay_appearance.invite_pos;
        const float lk_msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + lk_msg_height + global_style.WindowPadding.y;
    }
    break;
    default: PRINT_DEBUG("ERROR: unhandled notification type %i", (int)noti.type); break;
    }
    // add some y padding for niceness
    noti_height += 2 * global_style.WindowPadding.y;

    // For non-achievement types, prefer the cached rendered size from the previous frame
    // This ensures stacking is accurate even if the estimate was slightly off
    if (!is_achievement && noti.last_height > 0)
        noti_height = noti.last_height;

    // 0 on the y-axis is top, 0 on the x-axis is left
    float x = 0.0f;
    float y = 0.0f;
    float animate_size = 0.0f;
    const float margin_y = settings->overlay_appearance.notification_margin_y;
    const float margin_x = settings->overlay_appearance.notification_margin_x;

    switch (pos) {
    // top
    case Overlay_Appearance::NotificationPosition::top_left:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = margin_x - animate_size;
        y = coords.top_left.second + margin_y;
        coords.top_left.second = y + noti_height;
    break;
    case Overlay_Appearance::NotificationPosition::top_center:
        animate_size = animate_factor(elapsed, duration) * noti_height;
        x = (scrn_width / 2) - (noti_width / 2);
        y = coords.top_center.second + margin_y - animate_size;
        coords.top_center.second = y + noti_height;
    break;
    case Overlay_Appearance::NotificationPosition::top_right:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = (scrn_width - noti_width - margin_x) + animate_size;
        y = coords.top_right.second + margin_y;
        coords.top_right.second = y + noti_height;
    break;

    // bot
    case Overlay_Appearance::NotificationPosition::bot_left:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = margin_x - animate_size;
        y = scrn_height - coords.bot_left.second - margin_y - noti_height;
        coords.bot_left.second = scrn_height - y;
    break;
    case Overlay_Appearance::NotificationPosition::bot_center:
        animate_size = animate_factor(elapsed, duration) * noti_height;
        x = (scrn_width / 2) - (noti_width / 2);
        y = scrn_height - coords.bot_center.second - margin_y - noti_height + animate_size;
        coords.bot_center.second = scrn_height - y;
    break;
    case Overlay_Appearance::NotificationPosition::bot_right:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = (scrn_width - noti_width - margin_x) + animate_size;
        y = scrn_height - coords.bot_right.second - margin_y - noti_height;
        coords.bot_right.second = scrn_height - y;
    break;

    default: /* satisfy compiler warning */ break;
    }

    ImGui::SetNextWindowPos(ImVec2( x, y ));
    // Achievement notifications use fixed size (already sized correctly).
    // All other types auto-resize with 25% minimum width and 50% maximum width.
    if (is_achievement) {
        ImGui::SetNextWindowSize(ImVec2(min_noti_width, noti_height));
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_noti_width, 0), ImVec2(scrn_width * 0.5f, FLT_MAX));
    }
}

float Steam_Overlay::animate_factor(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration)
{
    if (settings->overlay_appearance.notification_animation <= 0) return 0.0f; // no animation

    std::chrono::milliseconds animation_duration(settings->overlay_appearance.notification_animation);
    // PRINT_DEBUG("ELAPSED %u/%u/%u", (uint32)elapsed.count(), (uint32)duration.count(), (uint32)animation_duration.count());

    float factor = 0.0f;
    if (elapsed < animation_duration) { // sliding in
        factor = 1.0f - (static_cast<float>(elapsed.count()) / animation_duration.count());
        // PRINT_DEBUG("SHOW FACTOR %f", factor);
    } else {
        // time between sliding in/out animation
        // here we add the animation duration because we want to count after the animation
        // if we have 1 sec animation & 2 sec show time:
        //   the duration will start at < 1 sec during the initial animation
        //   after the animation (1 sec), the duration will be >= 1 sec
        //   but since we want 2 sec show time, the duration must last 3 sec
        auto steady_time = animation_duration + duration;
        if (elapsed > steady_time) {
            factor = static_cast<float>((elapsed - steady_time).count()) / animation_duration.count();
            // PRINT_DEBUG("HIDE FACTOR %f", factor);
        }
    }
    
    return factor;
}

void Steam_Overlay::add_ach_progressbar(const Overlay_Achievement &ach)
{
    if (!ach.achieved && ach.progress > 0 && ach.max_progress > 0) {
        char buf[32]{};
        sprintf(buf, "%u/%u", ach.progress, ach.max_progress);
        ImGui::ProgressBar((float)ach.progress / ach.max_progress, { -1 , settings->overlay_appearance.font_size }, buf);
    }
}

ImVec4 Steam_Overlay::get_notification_bg_rgba_safe()
{
    if (settings->overlay_appearance.notification_r >= 0 &&
        settings->overlay_appearance.notification_g >= 0 &&
        settings->overlay_appearance.notification_b >= 0 &&
        settings->overlay_appearance.notification_a >= 0)
    {
        return ImVec4(
            settings->overlay_appearance.notification_r,
            settings->overlay_appearance.notification_g,
            settings->overlay_appearance.notification_b,
            settings->overlay_appearance.notification_a
        );
    }
    
    // fallback to dark-gray background
    return ImVec4(
        0.12f,
        0.14f,
        0.21f,
        1.0f
    );
}

void Steam_Overlay::build_notifications(float width, float height)
{
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    std::queue<Friend> friend_actions_temp{};

    ImGui::PushFont(font_notif, 0.0f);
    // Add window rounding
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, settings->overlay_appearance.notification_rounding);

    // Helper: render 48px avatar + 3-line friend info for a notification
    auto render_notif_friend_header = [&](uint64 friend_id) {
        if (!friend_id) return;

        // Look up friend in the friends map
        const Friend *frd_ptr = nullptr;
        friend_window_state *frd_state = nullptr;
        for (auto &[f, s] : friends) {
            if (f.id() == friend_id) {
                frd_ptr = &f;
                frd_state = &s;
                break;
            }
        }
        if (!frd_ptr) return;

        const float avatar_size = 48.0f;

        // Avatar
        bool has_avatar = try_load_avatar(*frd_state, frd_ptr->id());
        if (has_avatar && frd_state->avatar_resource && frd_state->avatar_resource->GetResourceId() != 0) {
            ImGui::Image(frd_state->avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
        } else {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), IM_COL32(60, 60, 80, 255));
            ImGui::Dummy(ImVec2(avatar_size, avatar_size));
        }
        ImGui::SameLine();

        ImVec2 text_start = ImGui::GetCursorPos();
        uint32 local_appid = settings->get_local_game_id().AppID();

        // Line 1: Name (ID: steamid)
        ImGui::SetCursorPos(text_start);
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "%s", frd_ptr->name().c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(ID: %llu)", (unsigned long long)frd_ptr->id());

        // Line 2: Playing AppName (AppID XXXX)
        ImGui::SetCursorPosX(text_start.x);
        if (frd_ptr->appid() != 0) {
            auto it2 = steam_preowned_app_ids.find(frd_ptr->appid());
            std::string game = (it2 != steam_preowned_app_ids.end()) ? it2->second : std::to_string(frd_ptr->appid());
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Playing %s (AppID %u)", game.c_str(), frd_ptr->appid());
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Online");
        }

        // Line 3: Lobby info (only if same app)
        ImGui::SetCursorPosX(text_start.x);
        if ((local_appid == frd_ptr->appid()) && frd_ptr->lobby_id() != 0) {
            Steam_Matchmaking *mm_n = get_steam_client()->steam_matchmaking;
            if (mm_n) {
                CSteamID frd_lobby((uint64)frd_ptr->lobby_id());
                int mc = mm_n->GetNumLobbyMembers(frd_lobby);
                int ml = mm_n->GetLobbyMemberLimit(frd_lobby);
                CSteamID owner = mm_n->GetLobbyOwner(frd_lobby);
                std::string owner_name;
                for (auto &[f2, s2] : friends) {
                    if (f2.id() == owner.ConvertToUint64()) { owner_name = f2.name(); break; }
                }
                if (owner_name.empty()) {
                    if (owner == settings->get_local_steam_id())
                        owner_name = settings->get_local_name();
                    else if (owner.ConvertToUint64() == frd_ptr->id())
                        owner_name = frd_ptr->name();
                    else
                        owner_name = std::to_string(owner.ConvertToUint64());
                }
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s - %llu (%d/%d - %s)",
                    (owner_name == frd_ptr->name()) ? "Has Lobby" : "In Lobby",
                    (unsigned long long)frd_ptr->lobby_id(), mc, ml, owner_name.c_str());
            }
        }

        ImGui::Separator();
    };
   
    NotificationsCoords coords{};
    for (auto it = notifications.begin(); it != notifications.end(); ++it) {
        auto noti_duration = get_notification_duration((notification_type)it->type);
        if (noti_duration.count() <= 0) {
            it->expired = true;
            continue;
        }

        // *2 for sliding in & out animation
        auto total_allowed_duration = noti_duration + std::chrono::milliseconds(settings->overlay_appearance.notification_animation * 2);
        auto elapsed_notif = now - it->start_time;
        if (elapsed_notif > total_allowed_duration) {
            it->expired = true;
            continue;
        }

        float settings_noti_alpha = settings->overlay_appearance.notification_a >= 0.0f && settings->overlay_appearance.notification_a <= 1.0f
            ? settings->overlay_appearance.notification_a
            : 1.0f;
        
        ImGui::PushStyleColor(ImGuiCol_Border, adjust_imgui_color_for_swapchain(ImVec4(0, 0, 0, settings_noti_alpha)));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, adjust_imgui_color_for_swapchain(get_notification_bg_rgba_safe()));
        ImGui::PushStyleColor(ImGuiCol_Text, adjust_imgui_color_for_swapchain(ImVec4(1.0f, 1.0f, 1.0f, settings_noti_alpha)));
       
        // some extra window flags for each notification type
        ImGuiWindowFlags extra_flags = ImGuiWindowFlags_NoFocusOnAppearing;
        switch ((notification_type)it->type) {
            // games like "Mafia Definitive Edition" will pause the entire game/scene if focus was stolen
            // be less intrusive for notifications that do not require interaction
            case notification_type::achievement_progress:
            case notification_type::achievement:
            case notification_type::auto_accept_invite:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::message:
                // If we have a friend pointer, make the notification interactive for "Open Chat" button
                if (it->frd)
                    extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
                else
                    extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::invite:
                // interactive: buttons remain clickable
                if (show_overlay) extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_join_request:
                // interactive: Accept/Decline buttons
                if (show_overlay) extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_join_request_response:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::lobby_kicked:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::friend_lobby_available:
                // interactive: Request to Join button
                if (show_overlay) extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_status:
                // non-interactive but always visible on top when overlay is open
                extra_flags |= ImGuiWindowFlags_NoInputs;
            break;

            default:
                PRINT_DEBUG("error unhandled flags for type %i", (int)it->type);
            break;
        }

        std::string wnd_name = "NotiPopupShow" + std::to_string(it->id);

        set_next_notification_pos({width, height}, elapsed_notif, noti_duration, *it, coords);
        ImGuiWindowFlags noti_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | extra_flags;
        {
            bool is_ach = ((notification_type)it->type == notification_type::achievement ||
                           (notification_type)it->type == notification_type::achievement_progress);
            if (!is_ach) noti_flags |= ImGuiWindowFlags_AlwaysAutoResize;
        }
        if (ImGui::Begin(wnd_name.c_str(), nullptr, noti_flags)) {
            // Cache actual rendered size for accurate stacking next frame
            ImVec2 win_sz = ImGui::GetWindowSize();
            it->last_width = win_sz.x;
            it->last_height = win_sz.y;
            switch ((notification_type)it->type) {
                case notification_type::achievement_progress:
                case notification_type::achievement: {
                    const auto &ach = it->ach.value();
                    auto &icon_rsrc = (notification_type)it->type == notification_type::achievement
                        ? ach.icon
                        : ach.icon_gray;
                    ImGui::Text("%s", ach.title.c_str());
                    if (icon_rsrc->GetResourceId() != 0 && ImGui::BeginTable("imgui_table", 2)) {
                        ImGui::TableSetupColumn("imgui_table_image", ImGuiTableColumnFlags_WidthFixed, settings->overlay_appearance.icon_size);
                        ImGui::TableSetupColumn("imgui_table_text");
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, settings->overlay_appearance.icon_size);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Image(icon_rsrc->GetResourceId(), ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size));

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", ach.description.c_str());

                        ImGui::EndTable();
                    } else {
                        ImGui::TextWrapped("%s", ach.description.c_str());
                    }

                    if ((notification_type)it->type == notification_type::achievement_progress) {
                        add_ach_progressbar(ach);
                    }
                }
                break;

                case notification_type::invite: {
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationJoin[current_language])) {
                        it->frd->second.window_state |= window_state_join;
                        friend_actions_temp.push(it->frd->first);
                        // when we click "accept game invite" from someone else, we want to remove this notification immediately since it's no longer relevant
                        // this assignment will make the notification elapsed time insanely large
                        it->start_time = {};
                    }
                }
                break;

                case notification_type::message:
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (it->frd) {
                        if (ImGui::Button("Open Chat")) {
                            it->frd->second.window_state |= window_state_show;
                            show_chat = true;
                            it->start_time = {}; // dismiss notification
                        }
                    }
                break;

                case notification_type::auto_accept_invite:
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type::lobby_join_request: {
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationJoin[current_language])) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        mm->AcceptLobbyJoinRequest(it->join_request_lobby_id, it->join_request_requester_id);
                        notified_lobby_join_requests.erase({it->join_request_lobby_id, it->join_request_requester_id});
                        it->start_time = {};
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(translationRefuse[current_language])) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        mm->DeclineLobbyJoinRequest(it->join_request_lobby_id, it->join_request_requester_id);
                        notified_lobby_join_requests.erase({it->join_request_lobby_id, it->join_request_requester_id});
                        it->start_time = {};
                    }
                }
                break;

                case notification_type::lobby_join_request_response:
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type::lobby_kicked:
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type::friend_lobby_available: {
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button("Request to Join")) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        PRINT_DEBUG("user requesting join to lobby %" PRIu64 " (friend %" PRIu64 ")", it->join_request_lobby_id, it->source_friend_id);
                        mm->JoinLobby(CSteamID((uint64)it->join_request_lobby_id));
                        it->start_time = {};
                    }
                }
                break;
                
                default:
                    PRINT_DEBUG("error unhandled notification for type %i", (int)it->type);
                break;
            }

        }

        ImGui::End();

        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar();
    ImGui::PopFont();

    // erase all notifications whose visible time exceeded the max
    notifications.erase(std::remove_if(notifications.begin(), notifications.end(), [this](const Notification &item) {
        if (item.expired) {
            PRINT_DEBUG("removing a notification");
            allow_renderer_frame_processing(false);
            // uncomment this block to restore app input focus
            switch ((notification_type)item.type) {
                // we want to restore focus for these ones
                case notification_type::invite:
                    obscure_game_input(false);
                break;

                case notification_type::lobby_join_request:
                    obscure_game_input(false);
                    // clean up tracking if expired without action
                    notified_lobby_join_requests.erase({item.join_request_lobby_id, item.join_request_requester_id});
                break;

                case notification_type::friend_lobby_available:
                    obscure_game_input(false);
                break;

                // not effective
                case notification_type::achievement_progress:
                case notification_type::achievement:
                case notification_type::auto_accept_invite:
                case notification_type::message:
                case notification_type::lobby_join_request_response:
                case notification_type::lobby_kicked:
                    // nothing
                break;

                default:
                    PRINT_DEBUG("error unhandled remove for type %i", (int)item.type);
                break;
            }

            return true;
        }
        
        return false;
    }), notifications.end());

    if (!friend_actions_temp.empty()) {
        while (!friend_actions_temp.empty()) {
            has_friend_action.push(friend_actions_temp.front());
            friend_actions_temp.pop();
        }
    }
}

void Steam_Overlay::add_auto_accept_invite_notification()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), "%s", translationAutoAcceptFriendInvite[current_language]);
    
    submit_notification(notification_type::auto_accept_invite, tmp);
    notify_sound_auto_accept_friend_invite();
}

void Steam_Overlay::add_invite_notification(std::pair<const Friend, friend_window_state>& wnd_state)
{
    if (settings->disable_overlay_friend_notification) return;
    
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    char tmp[TRANSLATION_BUFFER_SIZE]{};
    auto &first_friend = wnd_state.first;
    auto &name = first_friend.name();
    snprintf(tmp, sizeof(tmp), translationInvitedYouToJoinTheGame[current_language], name.c_str(), (uint64)first_friend.appid()); 
    
    submit_notification(notification_type::invite, tmp, &wnd_state);
}

void Steam_Overlay::post_achievement_notification(Overlay_Achievement &ach, bool for_progress)
{
    if (settings->disable_overlay_achievement_notification) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    bool achieved = !for_progress; // for progress notifications we want to load the gray icon
    // force upload to GPU if the pagination is request-based
    try_load_ach_icon(ach, achieved, settings->paginated_achievements_icons == 0);
    submit_notification(
        for_progress ? notification_type::achievement_progress : notification_type::achievement,
        ach.title + "\n" + ach.description,
        {},
        &ach
    );
}

// Per-display HDR + colour-space info queried from the OS for the overlay info panel.
struct DisplayHdrDetail_t {
    bool hdr_supported   = false;
    bool hdr_enabled     = false;
    bool wide_color      = false;   // WCG enforced (HDR10 WCG without full tone-mapping)
    bool force_disabled  = false;   // HDR suppressed by Windows for compatibility
    int  bpc             = 0;       // bits per colour channel; 0 = unknown
    int  sdr_white_nits  = -1;      // SDR white point in nits; -1 = unavailable
    char encoding[16]    = {};      // "RGB", "YCbCr444", "YCbCr422", "YCbCr420"
    char gamut[24]       = {};      // inferred: "BT.2020", "P3/BT.2020 WCG", "sRGB/BT.709"
    char transfer[16]    = {};      // inferred: "PQ/ST.2084", "sRGB-ext", "gamma 2.2"
    char range[8]        = {};      // inferred: "Full", "Limited", "?"
    char name[128]       = {};      // friendly monitor name (UTF-8)
};

static std::vector<DisplayHdrDetail_t> query_display_hdr_details()
{
    std::vector<DisplayHdrDetail_t> result;
#ifdef __WINDOWS__
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return result;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DisplayHdrDetail_t d{};

        // Advanced colour / HDR state
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO aci{};
        aci.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        aci.header.size      = sizeof(aci);
        aci.header.adapterId = paths[i].targetInfo.adapterId;
        aci.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&aci.header) == ERROR_SUCCESS) {
            d.hdr_supported  = aci.advancedColorSupported     != 0;
            d.hdr_enabled    = aci.advancedColorEnabled       != 0;
            d.wide_color     = aci.wideColorEnforced          != 0;
            d.force_disabled = aci.advancedColorForceDisabled != 0;
            d.bpc            = static_cast<int>(aci.bitsPerColorChannel);
            switch (aci.colorEncoding) {
                case DISPLAYCONFIG_COLOR_ENCODING_RGB:      strncpy(d.encoding, "RGB",      sizeof(d.encoding)-1); break;
                case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444: strncpy(d.encoding, "YCbCr444", sizeof(d.encoding)-1); break;
                case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422: strncpy(d.encoding, "YCbCr422", sizeof(d.encoding)-1); break;
                case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420: strncpy(d.encoding, "YCbCr420", sizeof(d.encoding)-1); break;
                default:                                     strncpy(d.encoding, "?",        sizeof(d.encoding)-1); break;
            }
        }

        // SDR white level (nits) — Windows 10 1809+
        // DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL = 11
        // Value: nits = SDRWhiteLevel * 80 / 1000  (1000 -> 80 nits standard, 2500 -> 200 nits)
        struct SdrWlReq { DISPLAYCONFIG_DEVICE_INFO_HEADER header; UINT32 SDRWhiteLevel; };
        SdrWlReq wl{};
        wl.header.type      = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(11);
        wl.header.size      = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS)
            d.sdr_white_nits = static_cast<int>(wl.SDRWhiteLevel * 80 / 1000);

        // Friendly monitor name
        DISPLAYCONFIG_TARGET_DEVICE_NAME tdn{};
        tdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tdn.header.size      = sizeof(tdn);
        tdn.header.adapterId = paths[i].targetInfo.adapterId;
        tdn.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tdn.header) == ERROR_SUCCESS && tdn.monitorFriendlyDeviceName[0])
            WideCharToMultiByte(CP_UTF8, 0, tdn.monitorFriendlyDeviceName, -1, d.name, sizeof(d.name)-1, nullptr, nullptr);
        if (!d.name[0])
            strncpy(d.name, "Unknown Display", sizeof(d.name)-1);

        // Inferred transfer function / gamma
        if (d.hdr_enabled)
            strncpy(d.transfer, "PQ/ST.2084", sizeof(d.transfer)-1);
        else if (d.wide_color)
            strncpy(d.transfer, "sRGB-ext", sizeof(d.transfer)-1);
        else
            strncpy(d.transfer, "gamma 2.2", sizeof(d.transfer)-1);

        // Inferred color gamut
        if (d.hdr_enabled)
            strncpy(d.gamut, "BT.2020", sizeof(d.gamut)-1);
        else if (d.wide_color)
            strncpy(d.gamut, "P3/BT.2020 WCG", sizeof(d.gamut)-1);
        else
            strncpy(d.gamut, "sRGB/BT.709", sizeof(d.gamut)-1);

        // Inferred color range (RGB on PC = full; YCbCr = limited)
        if (d.encoding[0] == 'R')
            strncpy(d.range, "Full", sizeof(d.range)-1);
        else if (d.encoding[0] == 'Y')
            strncpy(d.range, "Limited", sizeof(d.range)-1);
        else
            strncpy(d.range, "?", sizeof(d.range)-1);

        result.push_back(d);
    }
#endif
    return result;
}

// Query display HDR info, update s_sdr_white_scale, and mark the scale as queried.
// Returns the display list so the settings panel can display it without double-querying.
// Scale selection: prefers the first HDR-enabled display; falls back to the first display
// with any valid sdr_white_nits value; otherwise keeps the current scale.
static std::vector<DisplayHdrDetail_t> refresh_sdr_white_scale()
{
    auto displays = query_display_hdr_details();
    s_sdr_scale_queried = true;
    int best_nits = -1;
    // First pass: HDR-active display
    for (const auto& d : displays)
        if (d.hdr_enabled && d.sdr_white_nits > 0) { best_nits = d.sdr_white_nits; break; }
    // Second pass: any display with a valid reading
    if (best_nits < 0)
        for (const auto& d : displays)
            if (d.sdr_white_nits > 0) { best_nits = d.sdr_white_nits; break; }
    if (best_nits > 0)
        s_sdr_white_scale = best_nits / 80.0f;
    else
        s_sdr_white_scale = 1.0f; // fallback: standard 80 nits
    return displays;
}

// sRGB->linear decode before GPU upload.
// ingame_overlay forces DXGI_FORMAT_R8G8B8A8_UNORM on its own RTV so there is NO
// hardware sRGB encoding at the overlay render pass.  sRGB-encoded PNG bytes write
// as-is into the back buffer regardless of the swap-chain format.
//
// When to decode / adjust:
//   FP16/scRGB swap chain (R16G16B16A16_FLOAT)  — HDR path:
//     sRGB bytes are stored as linear-space floats and appear over-dark/over-saturated
//     without pre-decoding.  We decode sRGB->linear and then apply a pow(0.75) brightness
//     lift so shadow/midtone values are raised and the harsh contrast of the raw gamma-2.4
//     curve is reduced.
//   8-bit SDR (UNORM / UNORM_SRGB)  — SDR path:
//     UNORM bytes pass through to the display correctly, but a gentle 1.15× contrast boost
//     (applied in the sRGB domain around mid-grey) is added when running on a modern API
//     with a confirmed SDR swap chain.
//   HDR10 (R10G10B10A2, PQ gamma):
//     sRGB decode would produce wrong PQ colours.  NO decode, NO contrast.
//   DX9 / OpenGL: linear framebuffer, no sRGB path.  NO adjustment.
//
// Controlled by Overlay_Appearance::image_gamma (auto/on/off).
// "auto": HDR decode + lift ONLY when confirmed FP16/scRGB; SDR contrast on confirmed SDR.
// "on":   always apply HDR decode + lift.
// "off":  never adjust.

static void srgb_decode_pixels_if_needed(InGameOverlay::RendererHook_t *renderer,
                                         const Overlay_Appearance::SrgbDecode mode,
                                         uint8_t *rgba, size_t npixels)
{
    using RHT = InGameOverlay::RendererHookType_t;
    using SD  = Overlay_Appearance::SrgbDecode;

    // Determine which transform path to take based on the swap chain classification.
    // Possible paths:
    //   decode_linear_hdr  : sRGB→linear + SDR-white scale  (scRGB / FP16 / FP32 / 16-bit linear)
    //   decode_pq          : sRGB→linear→PQ encode + nit scale  (HDR10 PQ swap chains)
    //   decode_srgb_rtv    : sRGB→linear only (no scale)  (_SRGB back-buffer with hw encoding)
    //   sdr_contrast       : mild 1.15× contrast boost  (SDR UNORM, modern APIs)
    //   (none)             : raw pass-through
    bool decode_linear_hdr = false;
    bool decode_pq         = false;
    bool decode_srgb_rtv   = false;
    bool sdr_contrast      = false;

    bool modern_api = false;
    if (renderer) {
        switch (renderer->GetRendererHookType()) {
            case RHT::DirectX10:
            case RHT::DirectX11:
            case RHT::DirectX12:
            case RHT::Vulkan:
            case RHT::Metal:
                modern_api = true;
                break;
            default:
                break;
        }
    }

    const SwapchainColorSpace ecs = effective_swapchain_cs();

    if (mode == SD::On) {
        // Forced on: pick path based on effective colour space, or fall back to linear HDR decode.
        switch (ecs) {
            case SCS_HDR10_PQ:     decode_pq = true;         break;
            case SCS_SDR_SRGB_RTV: decode_srgb_rtv = true;   break;
            default:               decode_linear_hdr = true;  break;
        }
    } else if (mode == SD::Auto && modern_api) {
        switch (ecs) {
            case SCS_LINEAR_HDR:   decode_linear_hdr = true;  break;
            case SCS_HDR10_PQ:     decode_pq = true;          break;
            case SCS_SDR_SRGB_RTV: decode_srgb_rtv = true;    break;
            case SCS_SDR_UNORM:    sdr_contrast = true;       break;
            default: break; // SCS_UNKNOWN: no transform
        }
    }

    if (!decode_linear_hdr && !decode_pq && !decode_srgb_rtv && !sdr_contrast) return;

    if (decode_linear_hdr) {
        // HDR / FP16-scRGB path: sRGB→linear decode scaled by s_sdr_white_scale.
        // s_sdr_white_scale = sdr_white_nits / 80.0f (queried from the OS display API).
        // scRGB convention: 1.0 = 80 nits.  If Windows SDR white is 200 nits, the OS
        // scales SDR content by 2.5× when compositing.  We apply the same factor so our
        // overlay sits at the same perceived brightness as the game's own SDR UI.
        //
        // The overlay writes to a UNORM8 RTV so output is clamped to [0, 1].  Values
        // below the knee (0.75) pass through linearly — matching ImGui vertex-color
        // scaling.  Only values above the knee are softly compressed toward 1.0 using
        // a localised Reinhard shoulder, preserving mid-tone brightness while avoiding
        // hard clipping in highlights.
        static uint8_t hdr_lut[256];
        static float   hdr_lut_built_for = -1.0f;
        if (hdr_lut_built_for != s_sdr_white_scale) {
            hdr_lut_built_for = s_sdr_white_scale;
            constexpr float knee = 0.75f;
            constexpr float knee_range = 1.0f - knee;  // 0.25
            constexpr float knee_inv   = 1.0f / knee_range;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                l *= s_sdr_white_scale; // lift to match display SDR white brightness
                // Soft knee: only compress values above 0.75, keep mid-tones linear.
                if (l > knee) {
                    float x = (l - knee) * knee_inv;   // normalised excess [0, ∞)
                    l = knee + knee_range * (x / (1.0f + x)); // local Reinhard shoulder
                }
                hdr_lut[i] = (uint8_t)(fminf(l * 255.0f + 0.5f, 255.0f));
            }
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = hdr_lut[rgba[0]];
            rgba[1] = hdr_lut[rgba[1]];
            rgba[2] = hdr_lut[rgba[2]];
        }
    } else if (decode_pq) {
        // HDR10 PQ path: sRGB → linear → PQ (ST.2084) encode.
        // PQ reference: ITU-R BT.2100, SMPTE ST 2084.
        //   m1 = 2610 / 16384 = 0.1593017578125
        //   m2 = 2523 / 32    = 78.84375
        //   c1 = 3424 / 4096  = 0.8359375    (= c3 − c2 + 1)
        //   c2 = 2413 / 128   = 18.8515625
        //   c3 = 2392 / 128   = 18.6875
        // Input:  linear light normalised to 10000 nits  →  L = linear_nits / 10000.
        // Output: PQ value [0, 1] → scale to uint8 [0, 255].
        // SDR UI at s_sdr_white_scale*80 nits (e.g. 200 nits) → PQ ≈ 0.509 → uint8 ≈ 130.
        static uint8_t pq_lut[256];
        static float   pq_lut_built_for = -1.0f;
        if (pq_lut_built_for != s_sdr_white_scale) {
            pq_lut_built_for = s_sdr_white_scale;
            constexpr float m1 = 0.1593017578125f;
            constexpr float m2 = 78.84375f;
            constexpr float c1 = 0.8359375f;
            constexpr float c2 = 18.8515625f;
            constexpr float c3 = 18.6875f;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                // sRGB EOTF → linear [0,1] where 1 = diffuse white
                float lin = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                // Scale to absolute nits, then normalise to 10000.
                float nits = lin * s_sdr_white_scale * 80.0f; // e.g. 1.0 * 2.5 * 80 = 200 nits
                float Y = fminf(nits / 10000.0f, 1.0f);
                // PQ OETF
                float Ym1 = powf(Y, m1);
                float pq  = powf((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
                pq_lut[i] = (uint8_t)(fminf(pq * 255.0f + 0.5f, 255.0f));
            }
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = pq_lut[rgba[0]];
            rgba[1] = pq_lut[rgba[1]];
            rgba[2] = pq_lut[rgba[2]];
        }
    } else if (decode_srgb_rtv) {
        // _SRGB back-buffer path: the RTV has hardware sRGB encoding, so the GPU will
        // apply sRGB OETF on write.  If we feed sRGB bytes, they get double-encoded
        // (too dark).  Fix: decode to linear first; the hardware re-encodes to sRGB.
        // No SDR-white scale — this is an SDR display path.
        static uint8_t srgb_rtv_lut[256];
        static bool srgb_rtv_lut_ready = false;
        if (!srgb_rtv_lut_ready) {
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                srgb_rtv_lut[i] = (uint8_t)(fminf(l * 255.0f + 0.5f, 255.0f));
            }
            srgb_rtv_lut_ready = true;
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = srgb_rtv_lut[rgba[0]];
            rgba[1] = srgb_rtv_lut[rgba[1]];
            rgba[2] = srgb_rtv_lut[rgba[2]];
        }
    } else {
        // SDR path: mild contrast boost (~1.15×) in the sRGB domain around mid-grey.
        static uint8_t sdr_lut[256];
        static bool sdr_lut_ready = false;
        if (!sdr_lut_ready) {
            constexpr float kContrast = 1.15f;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s - 0.5f) * kContrast + 0.5f;
                l = (l < 0.f ? 0.f : (l > 1.f ? 1.f : l));
                sdr_lut[i] = (uint8_t)(l * 255.0f + 0.5f);
            }
            sdr_lut_ready = true;
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = sdr_lut[rgba[0]];
            rgba[1] = sdr_lut[rgba[1]];
            rgba[2] = sdr_lut[rgba[2]];
        }
    }
}

// Decode an sRGB float channel to linear and scale by s_sdr_white_scale.
// Used to adjust ImGui style colors for linear-HDR and PQ swap chains.
// For _SRGB back-buffers, decode to linear only (no scale).
static float srgb_ch_decode_scale(float c)
{
    float l = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    return l * s_sdr_white_scale;
}
// PQ OETF for a single float channel (used for ImGui colours on HDR10 PQ swap chains).
static float srgb_ch_to_pq(float c)
{
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    float lin = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    float nits = lin * s_sdr_white_scale * 80.0f;
    float Y = fminf(nits / 10000.0f, 1.0f);
    float Ym1 = powf(Y, m1);
    return powf((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
}
// sRGB→linear only (no scale) — for _SRGB back-buffer.
static float srgb_ch_decode_only(float c)
{
    return (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static ImVec4 adjust_imgui_color_for_swapchain(ImVec4 c)
{
    // Only transform when a non-SDR-UNORM colour space is confirmed.  Alpha preserved as-is.
    switch (effective_swapchain_cs()) {
        case SCS_LINEAR_HDR:
            return ImVec4(srgb_ch_decode_scale(c.x),
                          srgb_ch_decode_scale(c.y),
                          srgb_ch_decode_scale(c.z), c.w);
        case SCS_HDR10_PQ:
            return ImVec4(srgb_ch_to_pq(c.x),
                          srgb_ch_to_pq(c.y),
                          srgb_ch_to_pq(c.z), c.w);
        case SCS_SDR_SRGB_RTV:
            return ImVec4(srgb_ch_decode_only(c.x),
                          srgb_ch_decode_only(c.y),
                          srgb_ch_decode_only(c.z), c.w);
        default:
            return c;
    }
}

bool Steam_Overlay::try_load_ach_icon(Overlay_Achievement &ach, bool achieved, bool upload_new_icon_to_gpu)
{
    if (!_renderer) return false;
    // Don't cache images while the colour space is still unknown (detection pending).
    // Loading now would bake the wrong pixel transform into the cached decoded data.
    if (s_swapchain_cs == SCS_UNKNOWN && effective_swapchain_cs() == SCS_UNKNOWN) return false;
    if (settings->paginated_achievements_icons < 0) return false; // no icons are loaded anyway
    if (!settings->overlay_upload_achs_icons_to_gpu) return false; // don't upload anything to the GPU

    auto &icon_rsrc = achieved ? ach.icon : ach.icon_gray;
    if (icon_rsrc->GetResourceId() != 0) return true;

    // icons needs to be loaded, but we're not allowed
    if (!upload_new_icon_to_gpu) return false;

    int &icon_handle = achieved ? ach.icon_handle : ach.icon_gray_handle;
    if (Settings::UNLOADED_IMAGE_HANDLE == icon_handle) { // not loaded yet
        icon_handle = get_steam_client()->steam_user_stats->get_achievement_icon_handle(ach.name, achieved);
    }
    auto image_info = settings->get_image(icon_handle);
    if (image_info) {
        const int iw = static_cast<int>(image_info->width);
        const int ih = static_cast<int>(image_info->height);
        if (iw <= 0 || ih <= 0 || image_info->data.size() < (size_t)iw * ih * 4) return false;
        // Store in the struct — AttachResource holds a raw pointer, so the buffer must outlive the resource
        auto& icon_decoded_data = achieved ? ach.icon_decoded_data : ach.icon_gray_decoded_data;
        icon_decoded_data = image_info->data;
        srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
            (uint8_t*)icon_decoded_data.data(), (size_t)iw * ih);
        icon_rsrc->AttachResource((void*)icon_decoded_data.data(), (uint32_t)iw, (uint32_t)ih);
        
        PRINT_DEBUG("'%s' (%dx%d, result=%i)", ach.name.c_str(), iw, ih, (int)icon_rsrc->GetResourceId() != 0);
    }

    return icon_rsrc->GetResourceId() != 0;
}

bool Steam_Overlay::try_load_avatar(friend_window_state &state, uint64 steam_id)
{
    if (!_renderer) return false;
    // Don't cache images while the colour space is still unknown (detection pending).
    if (s_swapchain_cs == SCS_UNKNOWN && effective_swapchain_cs() == SCS_UNKNOWN) return false;
    
    // Already loaded?
    if (state.avatar_resource && state.avatar_resource->GetResourceId() != 0) return true;
    
    // Create resource if not exist
    if (!state.avatar_resource) {
        state.avatar_resource = _renderer->CreateResource();
        if (!state.avatar_resource) return false;
    }
    
    // Try to get avatar handle
    if (state.avatar_handle == -1) {
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        if (!steamFriends) return false;
        state.avatar_handle = steamFriends->GetMediumFriendAvatar(CSteamID(steam_id));
    }
    if (state.avatar_handle <= 0) return false;
    
    // Get image data
    Image_Data *img = settings->get_image(state.avatar_handle);
    if (!img || img->data.empty()) return false;
    
    int iw = static_cast<int>(img->width);
    int ih = static_cast<int>(img->height);
    if (iw <= 0 || ih <= 0 || img->data.size() < (size_t)iw * ih * 4) return false;
    
    // Store pixel data - AttachResource holds a raw pointer so buffer must outlive resource
    state.avatar_pixels = img->data;
    srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
        (uint8_t*)state.avatar_pixels.data(), (size_t)iw * ih);
    state.avatar_resource->AttachResource((void*)state.avatar_pixels.data(), (uint32_t)iw, (uint32_t)ih);
    
    return state.avatar_resource->GetResourceId() != 0;
}

bool Steam_Overlay::try_load_local_avatar()
{
    if (!_renderer) return false;
    // Don't cache images while the colour space is still unknown (detection pending).
    if (s_swapchain_cs == SCS_UNKNOWN && effective_swapchain_cs() == SCS_UNKNOWN) return false;
    
    // Already loaded?
    if (local_avatar_resource && local_avatar_resource->GetResourceId() != 0) return true;
    
    // Create resource if not exist
    if (!local_avatar_resource) {
        local_avatar_resource = _renderer->CreateResource();
        if (!local_avatar_resource) return false;
    }
    
    // Try to get avatar handle
    if (local_avatar_handle == -1) {
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        if (!steamFriends) return false;
        CSteamID local_id = settings->get_local_steam_id();
        local_avatar_handle = steamFriends->GetMediumFriendAvatar(local_id);
    }
    if (local_avatar_handle <= 0) return false;
    
    // Get image data
    Image_Data *img = settings->get_image(local_avatar_handle);
    if (!img || img->data.empty()) return false;
    
    int iw = static_cast<int>(img->width);
    int ih = static_cast<int>(img->height);
    if (iw <= 0 || ih <= 0 || img->data.size() < (size_t)iw * ih * 4) return false;
    
    // Store pixel data - AttachResource holds a raw pointer so buffer must outlive resource
    local_avatar_pixels = img->data;
    srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
        (uint8_t*)local_avatar_pixels.data(), (size_t)iw * ih);
    local_avatar_resource->AttachResource((void*)local_avatar_pixels.data(), (uint32_t)iw, (uint32_t)ih);
    
    return local_avatar_resource->GetResourceId() != 0;
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::overlay_render_proc()
{
    // When the ReShade addon is actively connected, it handles all rendering.
    // We still keep data structures alive so the bridge can read them.
    // If the addon stops calling (unloaded/disabled), the heartbeat goes stale
    // and the native overlay automatically resumes after the timeout.
    if (Bridge_IsConnected()) {
        // Even in bridge mode, render the tabbed chat window if any chats are open
        // (provides native chat UI alongside ReShade addon)
        std::lock_guard lock(overlay_mutex);
        if (Ready()) {
            build_chat_window();
        }
        return;
    }

    std::lock_guard lock(overlay_mutex);

    if (!Ready()) return;

    // Give the file-scope helper access to the current appearance settings
    // so effective_swapchain_cs() can resolve the user's Swapchain_Override.
    s_ov_app = &settings->overlay_appearance;

    // Deferred SCE texture cleanup - do this BEFORE any ImGui rendering
    // to avoid deleting resources mid-frame (which crashes DX9)
    if (sce_textures_pending_free) {
        sce_textures_pending_free = false;
        sce_textures_free_all();
    }

    // Drain the deferred display-info refresh (set on Reset/Removing).
    // Done here — on the render thread but outside the hooked resize path — so
    // QueryDisplayConfig does not block ResizeBuffers / device-release calls.
    if (s_pending_sdr_refresh) {
        s_pending_sdr_refresh = false;
        refresh_sdr_white_scale();
    }

    // Lazy-init SDR white scale from display info on the first frame.
    // Placed here because refresh_sdr_white_scale() is defined after the hook-ready
    // callback where arm_swapchain_format_detect is called.
    if (!s_sdr_scale_queried)
        refresh_sdr_white_scale();

    // HDR style color normalisation.
    // ingame_overlay forces DXGI_FORMAT_R8G8B8A8_UNORM on its own RTV, so ImGui vertex
    // colours (authored as sRGB 0-1 floats) are stored as raw UNORM floats when the
    // overlay is composited into the game's swap chain.  Depending on the colour space
    // (scRGB, HDR10 PQ, or _SRGB), ImGui style colours (authored as sRGB 0-1 floats)
    // must be transformed to match.  We patch all style colours for the duration of
    // this frame and restore them afterwards.
    ImVec4 saved_imgui_colors[ImGuiCol_COUNT];
    bool imgui_colors_patched = false;
    const SwapchainColorSpace ecs = effective_swapchain_cs();
    if (ecs == SCS_LINEAR_HDR || ecs == SCS_HDR10_PQ || ecs == SCS_SDR_SRGB_RTV) {
        ImVec4 *cols = ImGui::GetStyle().Colors;
        memcpy(saved_imgui_colors, cols, sizeof(saved_imgui_colors));
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            if (cols[i].w <= 0.f) continue; // skip fully transparent
            cols[i] = adjust_imgui_color_for_swapchain(cols[i]);
        }
        imgui_colors_patched = true;
    }

    if (show_overlay) {
        render_main_window();
    }

    if (stats.show_any_stats()) {
        // Give the stats HUD the same swapchain colour transform as the main overlay
        stats.color_transform = imgui_colors_patched ? adjust_imgui_color_for_swapchain : nullptr;
        stats.render_stats(current_language);
    }

    // Stats settings window (rendered when overlay is open)
    if (show_overlay) {
        stats.render_stats_settings(current_language);
    }

    // Notifications rendered LAST so they always draw on top of everything
    if (notifications.size()) {
        ImGuiIO &io = ImGui::GetIO();
        build_notifications(io.DisplaySize.x, io.DisplaySize.y);
    }

    if (imgui_colors_patched) {
        memcpy(ImGui::GetStyle().Colors, saved_imgui_colors, sizeof(saved_imgui_colors));
    }

    load_next_ach_icon();
}

uint32 Steam_Overlay::apply_global_style_color()
{
    uint32 style_color_stack = 0;
    if ((settings->overlay_appearance.background_r >= 0) &&
        (settings->overlay_appearance.background_g >= 0) &&
        (settings->overlay_appearance.background_b >= 0) &&
        (settings->overlay_appearance.background_a >= 0)) {
        ImVec4 colorSet = adjust_imgui_color_for_swapchain(ImVec4(
            settings->overlay_appearance.background_r,
            settings->overlay_appearance.background_g,
            settings->overlay_appearance.background_b,
            settings->overlay_appearance.background_a
        ));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colorSet);
        style_color_stack += 1;
    }

    if ((settings->overlay_appearance.element_r >= 0) &&
        (settings->overlay_appearance.element_g >= 0) &&
        (settings->overlay_appearance.element_b >= 0) &&
        (settings->overlay_appearance.element_a >= 0)) {
        ImVec4 colorSet = adjust_imgui_color_for_swapchain(ImVec4(
            settings->overlay_appearance.element_r,
            settings->overlay_appearance.element_g,
            settings->overlay_appearance.element_b,
            settings->overlay_appearance.element_a
        ));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_Button, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGrip, colorSet);
        style_color_stack += 4;
    }

    if ((settings->overlay_appearance.element_hovered_r >= 0) &&
        (settings->overlay_appearance.element_hovered_g >= 0) &&
        (settings->overlay_appearance.element_hovered_b >= 0) &&
        (settings->overlay_appearance.element_hovered_a >= 0)) {
        ImVec4 colorSet = adjust_imgui_color_for_swapchain(ImVec4(
            settings->overlay_appearance.element_hovered_r,
            settings->overlay_appearance.element_hovered_g,
            settings->overlay_appearance.element_hovered_b,
            settings->overlay_appearance.element_hovered_a
        ));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colorSet);
        style_color_stack += 4;
    }

    if ((settings->overlay_appearance.element_active_r >= 0) &&
        (settings->overlay_appearance.element_active_g >= 0) &&
        (settings->overlay_appearance.element_active_b >= 0) &&
        (settings->overlay_appearance.element_active_a >= 0)) {
        ImVec4 colorSet = adjust_imgui_color_for_swapchain(ImVec4(
            settings->overlay_appearance.element_active_r,
            settings->overlay_appearance.element_active_g,
            settings->overlay_appearance.element_active_b,
            settings->overlay_appearance.element_active_a
        ));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_Header, colorSet);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, colorSet);
        style_color_stack += 5;
    }

    return style_color_stack;
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::render_main_window()
{
    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), translationRenderer[current_language], (_renderer == nullptr ? "Unknown" : _renderer->GetLibraryName()));
    std::string windowTitle{};
    // Note: don't translate this, project and author names are nouns, they must be kept intact for proper referral
    // think of it as translating "Protobuf - Google"
    windowTitle.append("Ingame Overlay project - Nemirtingas (").append(tmp).append(")");

    bool show = true;

    ImGuiIO &io = ImGui::GetIO();

    ImGui::PushFont(font_default, 0.0f);
    uint32 style_color_stack = apply_global_style_color();

    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ io.DisplaySize.x, io.DisplaySize.y });
    if (ImGui::Begin(windowTitle.c_str(), &show,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        if (show_user_info) {
            // Show local user avatar next to info
            const float avatar_size = 48.0f;
            bool has_local_avatar = try_load_local_avatar();
            if (has_local_avatar && local_avatar_resource && local_avatar_resource->GetResourceId() != 0) {
                ImGui::Image(local_avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                ImGui::SameLine();
            }
            ImGui::LabelText("##playinglabel", translationUserPlaying[current_language],
                settings->get_local_name(),
                settings->get_local_steam_id().ConvertToUint64(),
                settings->get_local_game_id().AppID());
            
            // Show local IP addresses (one per adapter)
            if (network) {
                Networking::AdapterInfo local_adapters[16];
                int local_adapter_count = network->getAdapters(local_adapters, 16);
                for (int ai = 0; ai < local_adapter_count; ++ai) {
                    if (local_adapters[ai].ip == 0) continue;
                    char ip_buf[24];
                    format_ip_address(local_adapters[ai].ip, ip_buf, sizeof(ip_buf));
                    ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "%s: %s", local_adapters[ai].name, ip_buf);
                    ImGui::SameLine();
                    char btn_id[64];
                    snprintf(btn_id, sizeof(btn_id), "Copy##lip%d", ai);
                    if (ImGui::SmallButton(btn_id)) {
                        ImGui::SetClipboardText(ip_buf);
                    }
                }
            }

            // Show lobby/game status
            if (i_have_lobby) {
                Steam_Friends *steamFriends = get_steam_client()->steam_friends;
                std::string connect_str;
                if (steamFriends) {
                    connect_str = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
                }

                CSteamID lobby = settings->get_lobby();
                if (lobby.IsValid()) {
                    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
                    if (matchmaking) {
                        int member_count = matchmaking->GetNumLobbyMembers(lobby);
                        int member_limit = matchmaking->GetLobbyMemberLimit(lobby);
                        bool is_owner = (matchmaking->GetLobbyOwner(lobby) == settings->get_local_steam_id());
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "In Lobby (%d/%d) %s", 
                            member_count, member_limit,
                            is_owner ? "[Owner]" : "");
                        ImGui::SameLine();
                        char lobby_id_str[32];
                        snprintf(lobby_id_str, sizeof(lobby_id_str), "%llu", lobby.ConvertToUint64());
                        if (ImGui::SmallButton("Copy Lobby ID")) {
                            ImGui::SetClipboardText(lobby_id_str);
                        }
                    }
                } else if (!connect_str.empty()) {
                    // Connect string only (no formal lobby)
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Hosting Game");
                }

                if (!connect_str.empty()) {
                    // Get exe filename for the full launch command
                    std::string full_path = get_full_exe_path();
                    std::string::size_type pos = full_path.find_last_of("/\\");
                    std::string exe_name = (pos != std::string::npos) ? full_path.substr(pos + 1) : full_path;
                    
                    std::string launch_cmd = exe_name + " " + connect_str;
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Launch: %s", launch_cmd.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy##launch")) {
                        ImGui::SetClipboardText(launch_cmd.c_str());
                    }
                }
            }

            // Show game server info (listen server in same process)
            {
                Steam_GameServer *gs = get_steam_client()->steam_gameserver;
                if (gs && gs->BLoggedOn()) {
                    const auto &sd = gs->get_server_data();
                    std::string sname = sd.server_name();
                    std::string mname = sd.map_name();
                    uint32 num = sd.num_players();
                    uint32 maxp = sd.max_player_count();

                    if (!sname.empty() || !mname.empty() || maxp > 0) {
                        // Build: "ServerName - MapName (players/max)"
                        std::string server_line = "Server: ";
                        if (!sname.empty()) {
                            server_line += sname;
                            if (!mname.empty()) server_line += " - " + mname;
                        } else if (!mname.empty()) {
                            server_line += mname;
                        }
                        if (maxp > 0) {
                            char counts[32];
                            snprintf(counts, sizeof(counts), " (%u/%u)", num, maxp);
                            server_line += counts;
                        }
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", server_line.c_str());
                    }
                }
            }
        }

        ImGui::Spacing();
        
        ImGui::SameLine();
        // user clicked on "toggle user info"
        if (ImGui::Button(translationToggleUserInfo[current_language])) {
            show_user_info = !show_user_info;
        }

        ImGui::SameLine();
        // user clicked on "friends" - opens friends window
        {
            bool has_unread = false;
            for (auto &[frd, state] : friends) {
                if (state.window_state & window_state_need_attention) {
                    has_unread = true;
                    break;
                }
            }
            if (has_unread)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            if (ImGui::Button(translationFriends[current_language])) {
                show_friends = !show_friends;
            }
            if (has_unread)
                ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        // user clicked on "show achievements"
        if (ImGui::Button(translationShowAchievements[current_language])) {
            show_achievements = !show_achievements;
        }

        ImGui::SameLine();
        // user clicked on "copy id" on themselves
        if (ImGui::Button(translationCopyId[current_language])) {
            auto friend_id_str = std::to_string(settings->get_local_steam_id().ConvertToUint64());
            ImGui::SetClipboardText(friend_id_str.c_str());
        }

        ImGui::SameLine();
        // user clicked on "settings"
        if (ImGui::Button(translationSettings[current_language])) {
            show_settings = !show_settings;
        }

        ImGui::SameLine();
        // user clicked on "networks"
        if (ImGui::Button("Networks")) {
            show_networks = !show_networks;
        }

        // Lobby Chat button — only shown when in a lobby
        if (i_have_lobby) {
            ImGui::SameLine();
            if (ImGui::Button("Lobby Chat")) {
                show_lobby_chat = !show_lobby_chat;
            }
        }

        // SCE buttons — only shown when SCE catalog data is present
        {
            Steam_User_Stats *user_stats = get_steam_client()->steam_user_stats;
            if (user_stats->sce_data_populated && !user_stats->sce_game_data.series.empty()) {
                ImGui::SameLine();
                bool downloading = user_stats->sce_assets_downloading.load();
                if (!downloading) {
                    if (ImGui::Button("Download SCE Assets")) {
                        user_stats->RequestSceAssetDownload();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(show_sce_browser ? "[SCE Assets]" : "Browse SCE Assets"))
                        show_sce_browser = !show_sce_browser;
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Button("Downloading SCE Assets...");
                    ImGui::EndDisabled();
                }

                // ---- Download progress: fixed 25%-wide floating window ----
                if (downloading) {
                    uint32_t grand_dl    = user_stats->sce_assets_downloaded.load();
                    uint32_t grand_skip  = user_stats->sce_assets_skipped.load();
                    uint32_t grand_total = user_stats->sce_assets_total.load();
                    uint32_t grand_done  = grand_dl + grand_skip;

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

                    if (grand_total > 0) {
                        char total_lbl[128]{};
                        snprintf(total_lbl, sizeof(total_lbl),
                            "Total: %u downloaded, %u cached of %u",
                            grand_dl, grand_skip, grand_total);
                        float frac = (float)grand_done / (float)grand_total;
                        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), total_lbl);
                    } else {
                        ImGui::TextUnformatted("Collecting asset URLs...");
                    }

                    ImGui::Spacing();
                    for (int i = 0; i < Steam_User_Stats::SCE_NUM_TYPES; ++i) {
                        uint32_t tot = user_stats->sce_type_progress[i].total.load();
                        if (tot == 0) continue;
                        uint32_t cur = user_stats->sce_type_progress[i].current.load();
                        uint32_t dl  = user_stats->sce_type_progress[i].downloaded.load();
                        char lbl[128]{};
                        snprintf(lbl, sizeof(lbl), "%s: %u of %u (%u new)",
                            Steam_User_Stats::SCE_TYPE_LABELS[i], cur, tot, dl);
                        ImGui::ProgressBar((float)cur / (float)tot, ImVec2(-1.0f, 0.0f), lbl);
                    }

                    ImGui::End();
                }
            }

            // flush pending completion notification
            if (sce_asset_progress.pending_notification) {
                sce_asset_progress.pending_notification = false;
                char msg[256]{};
                snprintf(msg, sizeof(msg),
                    "SCE assets ready: %u downloaded, %u already cached (%u total)",
                    sce_asset_progress.downloaded,
                    sce_asset_progress.skipped,
                    sce_asset_progress.total);
                submit_notification(notification_type::message, msg);
                allow_renderer_frame_processing(false);
            }
        }
        
        ImGui::Spacing();
        ImGui::Spacing();
        // Stats settings button (replaces individual FPS/Frametime/Playtime checkboxes)
        ImGui::SameLine();
        if (ImGui::Button("Stats Settings")) {
            stats.show_stats_settings = !stats.show_stats_settings;
        }
        ImGui::SameLine();
        // Quick status indicators
        {
            char indicator[64] = {};
            bool any = false;
            if (stats.show_fps) { strcat(indicator, "FPS"); any = true; }
            if (stats.show_frametime) { if (any) strcat(indicator, ", "); strcat(indicator, "FT"); any = true; }
            if (stats.show_playtime) { if (any) strcat(indicator, ", "); strcat(indicator, "PT"); any = true; }
            if (any) {
                char wrapped[72];
                snprintf(wrapped, sizeof(wrapped), "(%s)", indicator);
                ImGui::TextDisabled("%s", wrapped);
            }
        }

        // --- Rendering info panel -------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        {
            using RHT = InGameOverlay::RendererHookType_t;

            // -- API / renderer lib --
            const char* api_name = "Unknown";
            if (_renderer) {
                switch (_renderer->GetRendererHookType()) {
                    case RHT::DirectX9:  api_name = "DirectX 9";  break;
                    case RHT::DirectX10: api_name = "DirectX 10"; break;
                    case RHT::DirectX11: api_name = "DirectX 11"; break;
                    case RHT::DirectX12: api_name = "DirectX 12"; break;
                    case RHT::OpenGL:    api_name = "OpenGL";     break;
                    case RHT::Vulkan:    api_name = "Vulkan";     break;
                    case RHT::Metal:     api_name = "Metal";      break;
                    default: break;
                }
            }
            ImGui::TextDisabled("API        : %s  (%s)",
                api_name, _renderer ? _renderer->GetLibraryName() : "None");

            ImGui::TextDisabled("Emu build  : " EMU_BUILD_STRING);
            ImGui::TextDisabled("Build date : " EMU_BUILD_DATE_STRING);

            // -- Game swapchain --
            bool hdr_api = false;
            if (_renderer) {
                auto rt = _renderer->GetRendererHookType();
                hdr_api = (rt == RHT::DirectX10 || rt == RHT::DirectX11 ||
                           rt == RHT::DirectX12 || rt == RHT::Vulkan || rt == RHT::Metal);
            }
            if (hdr_api) {
                ImGui::TextDisabled("Swapchain  : %s", s_swapchain_fmt_str);
                ImGui::TextDisabled("           : %s", s_swapchain_type_str);
            } else {
                ImGui::TextDisabled("Swapchain  : N/A  (DX9/OpenGL — linear framebuffer, no HDR path)");
            }

            // -- sRGB correction --
            using SD = Overlay_Appearance::SrgbDecode;
            const char* corr_str;
            const char* corr_reason;
            if (settings->overlay_appearance.image_gamma == SD::On) {
                corr_str    = "ON  (forced)";
                corr_reason = "Image_Gamma=on in config";
            } else if (settings->overlay_appearance.image_gamma == SD::Off) {
                corr_str    = "OFF (forced)";
                corr_reason = "Image_Gamma=off in config";
            } else if (!hdr_api) {
                corr_str    = "OFF";
                corr_reason = "DX9/OpenGL — legacy, no colour-space transform";
            } else {
                const SwapchainColorSpace ecs = effective_swapchain_cs();
                switch (ecs) {
                    case SCS_LINEAR_HDR:
                        corr_str    = "ON ";
                        corr_reason = "linear HDR (FP16/FP32/UNORM16) — sRGB->linear + SDR-white scale";
                        break;
                    case SCS_HDR10_PQ:
                        corr_str    = "ON ";
                        corr_reason = "HDR10 PQ — sRGB->linear->PQ (ST.2084) encode";
                        break;
                    case SCS_SDR_SRGB_RTV:
                        corr_str    = "ON ";
                        corr_reason = "_SRGB back-buffer — sRGB->linear (hw re-encodes)";
                        break;
                    case SCS_SDR_UNORM:
                        corr_str    = "OFF";
                        corr_reason = "SDR UNORM — bytes pass through unchanged";
                        break;
                    default:
                        corr_str    = "OFF";
                        corr_reason = "awaiting swap chain format detection";
                        break;
                }
            }
            ImGui::TextDisabled("sRGB corr. : %s  — %s", corr_str, corr_reason);
            {
                int so = static_cast<int>(settings->overlay_appearance.swapchain_override);
                if (so > 0) {
                    static const char* ov_names[] = { "auto", "linear_hdr", "hdr10_pq", "srgb_rtv", "sdr" };
                    const char* ov_name = (so >= 1 && so <= 4) ? ov_names[so] : "?";
                    ImGui::TextDisabled("           : Swapchain_Override=%s (user config)", ov_name);
                }
            }

            // -- Per-display info (queried once per launch; also updates s_sdr_white_scale) --
            static bool displays_queried = false;
            static std::vector<DisplayHdrDetail_t> displays;
            if (!displays_queried) {
                displays         = refresh_sdr_white_scale();
                displays_queried = true;
            }
            if (displays.empty()) {
                ImGui::TextDisabled("Display    : N/A");
            } else {
                for (int di = 0; di < (int)displays.size() && di < 4; ++di) {
                    const auto& d = displays[di];
                    const char* hdr_st;
                    if      (!d.hdr_supported)  hdr_st = "SDR only";
                    else if (d.force_disabled)  hdr_st = "HDR suppressed";
                    else if (d.wide_color && !d.hdr_enabled) hdr_st = "WCG (no HDR)";
                    else if (d.hdr_enabled)     hdr_st = "HDR ON";
                    else                        hdr_st = "HDR OFF (supported)";
                    char bpc_buf[8] = "?";
                    if (d.bpc > 0) snprintf(bpc_buf, sizeof(bpc_buf), "%d", d.bpc);
                    char white_buf[24] = "?";
                    if (d.sdr_white_nits >= 0)
                        snprintf(white_buf, sizeof(white_buf), "%d nits", d.sdr_white_nits);
                    ImGui::TextDisabled("Display %d  : %s  |  %s  |  %sbpc  |  SDR white: %s",
                        di+1, d.name, hdr_st, bpc_buf, white_buf);
                    ImGui::TextDisabled("           : gamut: %s  |  TF: %s  |  range: %s  |  enc: %s",
                        d.gamut, d.transfer, d.range, d.encoding);
                }
            }

            if (ImGui::SmallButton("Refresh##hdr_info")) {
                displays = refresh_sdr_white_scale();
                displays_queried = true;
                if (_renderer)
                    arm_swapchain_format_detect(_renderer);
            }
            { const auto ecs = effective_swapchain_cs();
            if (ecs == SCS_LINEAR_HDR || ecs == SCS_HDR10_PQ)
                ImGui::TextDisabled("HDR scale  : %.2fx  (SDR white = %d nits)",
                    s_sdr_white_scale, (int)(s_sdr_white_scale * 80.f + 0.5f));
            }
        }
        ImGui::Separator();
        // -------------------------------------------------------------------

        // Friends window (separate from main overlay)
        if (show_friends) {
            const float min_w = io.DisplaySize.x * 0.25f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, ImGui::GetFontSize() * 16), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationFriends[current_language], &show_friends)) {

                // ---- Local user header: 64px avatar + 3 info lines ----
                {
                    const float avatar_size = 64.0f;
                    bool has_local_avatar = try_load_local_avatar();
                    if (has_local_avatar && local_avatar_resource && local_avatar_resource->GetResourceId() != 0) {
                        ImGui::Image(local_avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                    } else {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), IM_COL32(60, 60, 80, 255));
                        ImGui::Dummy(ImVec2(avatar_size, avatar_size));
                    }
                    ImGui::SameLine();

                    // 3 lines to the right of avatar
                    ImVec2 text_start = ImGui::GetCursorPos();
                    uint32 local_appid = settings->get_local_game_id().AppID();

                    auto resolve_app_name = [](uint32 appid) -> std::string {
                        auto it = steam_preowned_app_ids.find(appid);
                        if (it != steam_preowned_app_ids.end()) return it->second;
                        return std::to_string(appid);
                    };

                    // Line 1: Username (ID: steamid)
                    ImGui::SetCursorPos(text_start);
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "%s", settings->get_local_name());
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(ID: %llu)",
                        settings->get_local_steam_id().ConvertToUint64());

                    // Line 2: Playing AppName (AppID XXXX)
                    ImGui::SetCursorPosX(text_start.x);
                    std::string app_name = resolve_app_name(local_appid);
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Playing %s (AppID %u)", app_name.c_str(), local_appid);

                    // Line 3: In Lobby / In Server / In Game
                    ImGui::SetCursorPosX(text_start.x);
                    CSteamID lobby = settings->get_lobby();
                    bool in_lobby = lobby.IsValid();
                    Steam_GameServer *gs = get_steam_client()->steam_gameserver;
                    bool in_server = gs && gs->BLoggedOn();

                    if (in_lobby) {
                        Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
                        if (matchmaking) {
                            int member_count = matchmaking->GetNumLobbyMembers(lobby);
                            int member_limit = matchmaking->GetLobbyMemberLimit(lobby);
                            CSteamID owner = matchmaking->GetLobbyOwner(lobby);
                            std::string owner_name;
                            for (auto &[frd, st] : friends) {
                                if (frd.id() == owner.ConvertToUint64()) {
                                    owner_name = frd.name();
                                    break;
                                }
                            }
                            if (owner_name.empty()) {
                                if (owner == settings->get_local_steam_id())
                                    owner_name = settings->get_local_name();
                                else
                                    owner_name = std::to_string(owner.ConvertToUint64());
                            }
                            bool local_is_owner = (owner == settings->get_local_steam_id());
                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s - %llu (%d/%d - %s)",
                                local_is_owner ? "Has Lobby" : "In Lobby",
                                lobby.ConvertToUint64(), member_count, member_limit, owner_name.c_str());
                        }
                    } else if (in_server) {
                        const auto &sd = gs->get_server_data();
                        std::string sname = sd.server_name();
                        if (!sname.empty())
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "In Server - %s", sname.c_str());
                        else
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "In Server");
                    } else {
                        ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "In Game");
                    }
                }

                // ---- Dynamic action buttons ----
                {
                    CSteamID lobby = settings->get_lobby();
                    bool is_lobby_owner = false;
                    if (lobby.IsValid()) {
                        Steam_Matchmaking *mm_chk = get_steam_client()->steam_matchmaking;
                        if (mm_chk) is_lobby_owner = (mm_chk->GetLobbyOwner(lobby) == settings->get_local_steam_id());
                    }
                    if (i_have_lobby && is_lobby_owner && !friends.empty()) {
                        std::string inviteAll(translationInviteAll[current_language]);
                        inviteAll.append("##PopupInviteAllFriends");
                        if (ImGui::Button(inviteAll.c_str())) {
                            invite_all_friends_clicked = true;
                        }
                        ImGui::SameLine();
                    }
                    if (lobby.IsValid()) {
                        if (ImGui::Button("Copy Lobby ID##fl")) {
                            ImGui::SetClipboardText(std::to_string(lobby.ConvertToUint64()).c_str());
                        }
                        ImGui::SameLine();
                        // "Remove All from Lobby" — owner only
                        Steam_Matchmaking *mm_btns = get_steam_client()->steam_matchmaking;
                        if (mm_btns && mm_btns->GetLobbyOwner(lobby) == settings->get_local_steam_id()) {
                            if (mm_btns->GetNumLobbyMembers(lobby) > 1) {
                                if (ImGui::Button("Remove All from Lobby##fl")) {
                                    mm_btns->KickAllLobbyMembers(lobby.ConvertToUint64());
                                }
                                ImGui::SameLine();
                            }
                        } else {
                            if (ImGui::Button("Leave Lobby##fl")) {
                                mm_btns->LeaveLobby(lobby);
                            }
                            ImGui::SameLine();
                        }
                    }
                    if (ImGui::Button(translationCopyId[current_language])) {
                        ImGui::SetClipboardText(std::to_string(settings->get_local_steam_id().ConvertToUint64()).c_str());
                    }
                }
                ImGui::Separator();

                if (!friends.empty()) {

                    // ---- Partition friends into In Game (same app) / Online (different app) ----
                    struct FriendEntry {
                        const Friend *frd;
                        friend_window_state *state;
                    };
                    std::vector<FriendEntry> in_game_friends, online_friends;
                    uint32 local_appid = settings->get_local_game_id().AppID();
                    for (auto &[frd, state] : friends) {
                        if (frd.appid() == local_appid)
                            in_game_friends.push_back({&frd, &state});
                        else
                            online_friends.push_back({&frd, &state});
                    }

                    // ---- App name resolver ----
                    auto resolve_app_name = [](uint32 appid) -> std::string {
                        auto it = steam_preowned_app_ids.find(appid);
                        if (it != steam_preowned_app_ids.end()) return it->second;
                        return std::to_string(appid);
                    };

                    // ---- Per-friend renderer (3-line: avatar + name/id + game + lobby) ----
                    auto render_friend_row = [&](const Friend &frd, friend_window_state &state) {
                        ImGui::PushID(state.id - base_friend_window_id + base_friend_item_id);

                        // 48px avatar to fit 3 text lines
                        const float avatar_size = 48.0f;
                        float line_h = ImGui::GetTextLineHeight();
                        float row_height = (std::max)(avatar_size, line_h * 3.0f);
                        ImVec2 cursor_before = ImGui::GetCursorPos();
                        ImGui::Selectable("##friend_row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_height));
                        // Capture right-click on the full selectable row for context menu
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            ImGui::OpenPopup("##ctx_friend");
                        ImGui::SetCursorPos(cursor_before);

                        // Avatar
                        bool has_avatar = try_load_avatar(state, frd.id());
                        if (has_avatar && state.avatar_resource && state.avatar_resource->GetResourceId() != 0) {
                            ImGui::Image(state.avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                        } else {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), IM_COL32(60, 60, 80, 255));
                            ImGui::Dummy(ImVec2(avatar_size, avatar_size));
                        }
                        ImGui::SameLine();

                        ImVec2 text_start = ImGui::GetCursorPos();

                        // Line 1: FriendName (ID: steamid)
                        ImGui::SetCursorPos(text_start);
                        bool needs_attn = (state.window_state & window_state_need_attention);
                        if (needs_attn)
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", frd.name().c_str());
                        else
                            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "%s", frd.name().c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(ID: %llu)", (unsigned long long)frd.id());
                        // Show detected IPs if available
                        if (network) {
                            uint32 frd_ips[16];
                            int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
                            for (int ipi = 0; ipi < frd_ip_count; ++ipi) {
                                if (frd_ips[ipi] == 0) continue;
                                char ip_buf[24];
                                format_ip_address(frd_ips[ipi], ip_buf, sizeof(ip_buf));
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "[%s]", ip_buf);
                            }
                        }

                        // Line 2: Playing AppName (AppID XXXX)
                        ImGui::SetCursorPosX(text_start.x);
                        if (frd.appid() != 0) {
                            std::string game = resolve_app_name(frd.appid());
                            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Playing %s (AppID %u)", game.c_str(), frd.appid());
                        } else {
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Online");
                        }

                        // Line 3: Lobby info (if friend has a lobby)
                        if (frd.lobby_id() != 0) {
                            ImGui::SetCursorPosX(text_start.x);
                            int frd_mc = 0, frd_ml = 0;
                            std::string frd_owner_name;

                            // Try local matchmaking data first (available for same-app lobbies)
                            Steam_Matchmaking *mm_frd = get_steam_client()->steam_matchmaking;
                            if (mm_frd) {
                                CSteamID frd_lobby((uint64)frd.lobby_id());
                                frd_mc = mm_frd->GetNumLobbyMembers(frd_lobby);
                                frd_ml = mm_frd->GetLobbyMemberLimit(frd_lobby);
                                CSteamID frd_owner = mm_frd->GetLobbyOwner(frd_lobby);
                                if (frd_owner.IsValid()) {
                                    for (auto &[f2, s2] : friends) {
                                        if (f2.id() == frd_owner.ConvertToUint64()) { frd_owner_name = f2.name(); break; }
                                    }
                                    if (frd_owner_name.empty()) {
                                        if (frd_owner == settings->get_local_steam_id())
                                            frd_owner_name = settings->get_local_name();
                                        else if (frd_owner.ConvertToUint64() == frd.id())
                                            frd_owner_name = frd.name();
                                    }
                                }
                            }

                            // Fall back to proto fields for cross-app lobbies
                            if (frd_mc == 0 && frd.lobby_member_count() > 0)
                                frd_mc = frd.lobby_member_count();
                            if (frd_ml == 0 && frd.lobby_member_limit() > 0)
                                frd_ml = frd.lobby_member_limit();
                            if (frd_owner_name.empty() && frd.lobby_owner_name().size() > 0)
                                frd_owner_name = frd.lobby_owner_name();

                            // Check if this friend is the lobby owner
                            bool friend_is_owner = (!frd_owner_name.empty() && frd_owner_name == frd.name());

                            if (!frd_owner_name.empty())
                                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s - %llu (%d/%d - %s)",
                                    friend_is_owner ? "Has Lobby" : "In Lobby",
                                    (unsigned long long)frd.lobby_id(), frd_mc, frd_ml, frd_owner_name.c_str());
                            else
                                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "In Lobby - %llu (%d/%d)",
                                    (unsigned long long)frd.lobby_id(), frd_mc, frd_ml);
                        }

                        // Right-click context menu (opened from selectable above)
                        if (ImGui::BeginPopup("##ctx_friend")) {
                            if (ImGui::MenuItem(translationChat[current_language])) {
                                state.window_state |= window_state_show;
                                show_chat = true;
                            }
                            bool same_app = (settings->get_local_game_id().AppID() == frd.appid());
                            bool is_my_lobby_owner = false;
                            bool friend_in_my_lobby = false;
                            {
                                CSteamID my_lob = settings->get_lobby();
                                if (my_lob.IsValid()) {
                                    Steam_Matchmaking *mm_ctx = get_steam_client()->steam_matchmaking;
                                    if (mm_ctx) {
                                        is_my_lobby_owner = (mm_ctx->GetLobbyOwner(my_lob) == settings->get_local_steam_id());
                                        int mc = mm_ctx->GetNumLobbyMembers(my_lob);
                                        for (int mi = 0; mi < mc; ++mi) {
                                            if (mm_ctx->GetLobbyMemberByIndex(my_lob, mi).ConvertToUint64() == frd.id()) {
                                                friend_in_my_lobby = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            if (same_app && i_have_lobby && is_my_lobby_owner && !friend_in_my_lobby) {
                                if (ImGui::MenuItem(translationInvite[current_language])) {
                                    state.window_state |= window_state_invite;
                                    has_friend_action.push(frd);
                                }
                            }
                            if (same_app && state.joinable && frd.lobby_id() != 0 && !friend_in_my_lobby) {
                                if (ImGui::MenuItem(translationJoin[current_language])) {
                                    state.window_state |= window_state_join;
                                    has_friend_action.push(frd);
                                }
                            }
                            // Show Accept Invite if this friend sent us a pending invite we haven't accepted yet
                            if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                                if (ImGui::MenuItem("Accept Invite")) {
                                    state.window_state |= window_state_join;
                                    has_friend_action.push(frd);
                                }
                            }
                            if (ImGui::MenuItem(translationCopyId[current_language])) {
                                ImGui::SetClipboardText(std::to_string(frd.id()).c_str());
                            }
                            if (network) {
                                uint32 frd_ips[16];
                                int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
                                if (frd_ip_count == 1 && frd_ips[0] != 0) {
                                    char ip_buf[24];
                                    format_ip_address(frd_ips[0], ip_buf, sizeof(ip_buf));
                                    if (ImGui::MenuItem("Copy IP")) {
                                        ImGui::SetClipboardText(ip_buf);
                                    }
                                } else if (frd_ip_count > 1) {
                                    if (ImGui::BeginMenu("Copy IP")) {
                                        for (int ipi = 0; ipi < frd_ip_count; ++ipi) {
                                            if (frd_ips[ipi] == 0) continue;
                                            char ip_buf[24];
                                            format_ip_address(frd_ips[ipi], ip_buf, sizeof(ip_buf));
                                            if (ImGui::MenuItem(ip_buf)) {
                                                ImGui::SetClipboardText(ip_buf);
                                            }
                                        }
                                        ImGui::EndMenu();
                                    }
                                }
                            }
                            if (frd.lobby_id() != 0) {
                                if (ImGui::MenuItem("Copy Lobby ID")) {
                                    ImGui::SetClipboardText(std::to_string(frd.lobby_id()).c_str());
                                }
                            }
                            // Kick from lobby (only if friend is actually in our lobby member list and we're the owner)
                            {
                                CSteamID my_lobby = settings->get_lobby();
                                if (my_lobby.IsValid()) {
                                    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                                    CSteamID owner = mm->GetLobbyOwner(my_lobby);
                                    if (owner == settings->get_local_steam_id()) {
                                        // Check actual lobby member list
                                        bool friend_in_my_lobby = false;
                                        int member_count = mm->GetNumLobbyMembers(my_lobby);
                                        for (int mi = 0; mi < member_count; ++mi) {
                                            if (mm->GetLobbyMemberByIndex(my_lobby, mi).ConvertToUint64() == frd.id()) {
                                                friend_in_my_lobby = true;
                                                break;
                                            }
                                        }
                                        if (friend_in_my_lobby) {
                                            ImGui::Separator();
                                            if (ImGui::MenuItem("Kick from Lobby")) {
                                                mm->KickLobbyMember(my_lobby.ConvertToUint64(), frd.id());
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                    };

                    // ---- Render grouped friend list ----
                    ImGui::BeginChild("##friends_child", ImVec2(0, 0), true);

                    // In Game section
                    if (!in_game_friends.empty()) {
                        char hdr[64];
                        snprintf(hdr, sizeof(hdr), "In Game (%d)##frd_ingame", (int)in_game_friends.size());
                        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.15f, 0.35f, 0.15f, 0.80f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.20f, 0.45f, 0.20f, 0.90f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.25f, 0.55f, 0.25f, 1.00f));
                        bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (auto &e : in_game_friends) render_friend_row(*e.frd, *e.state);
                        }
                    }

                    // Online section
                    if (!online_friends.empty()) {
                        char hdr[64];
                        snprintf(hdr, sizeof(hdr), "Online (%d)##frd_online", (int)online_friends.size());
                        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.20f, 0.30f, 0.45f, 0.80f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.25f, 0.38f, 0.55f, 0.90f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.30f, 0.45f, 0.65f, 1.00f));
                        bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (auto &e : online_friends) render_friend_row(*e.frd, *e.state);
                        }
                    }

                    ImGui::EndChild();
                }
            }
            ImGui::End();
        }

        // Chat window (tabbed, separate from friends list)
        build_chat_window();

        // user clicked on "show achievements" button
        if (show_achievements && achievements.size()) {
            const float noti_w = io.DisplaySize.x * Notification::width_percent;
            const float min_w = std::max(ImGui::GetFontSize() * 32, noti_w);
            ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationAchievementWindow[current_language], &show_achievements)) {
                // --- total completion progress bar ---
                {
                    int total = (int)achievements.size();
                    int done = 0;
                    for (const auto &a : achievements) if (a.achieved) ++done;
                    float fill = total > 0 ? (float)done / total : 0.0f;
                    float pct  = fill * 100.0f;

                    char left_buf[32]{};
                    snprintf(left_buf, sizeof(left_buf), "%d/%d", done, total);
                    char right_buf[32]{};
                    snprintf(right_buf, sizeof(right_buf), "%.1f%%", pct);

                    const float bar_h = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
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

                    // left: x/y
                    ImVec2 left_sz = ImGui::CalcTextSize(left_buf);
                    ImVec2 left_pos = { bar_pos.x + 4.0f, bar_pos.y + (bar_h - left_sz.y) * 0.5f };
                    draw_sh(left_pos, left_buf);

                    // right: percentage
                    ImVec2 right_sz = ImGui::CalcTextSize(right_buf);
                    ImVec2 right_pos = { bar_pos.x + bar_width - right_sz.x - 4.0f, bar_pos.y + (bar_h - right_sz.y) * 0.5f };
                    draw_sh(right_pos, right_buf);
                }

                // --- Reset / Simulate buttons ---
                if (ImGui::Button("Reset##ach_reset") && !achievements_snapshot.empty()) {
                    for (auto &ax : achievements) {
                        for (const auto &snap : achievements_snapshot) {
                            if (snap.name == ax.name) {
                                ax.achieved    = snap.achieved;
                                ax.progress    = snap.progress;
                                ax.unlock_time = snap.unlock_time;
                                break;
                            }
                        }
                    }
                    ach_global_percentages = ach_global_percentages_snapshot;
                }
                ImGui::SameLine();
                if (ImGui::Button(translationTestAchievement[current_language])) {
                    show_test_achievement();
                }
                ImGui::SameLine();
                if (ImGui::Button("Simulate##ach_simulate")) {
                    // random seed based on current time
                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_int_distribution<uint32_t> percent_dist(0, 100);
                    std::uniform_real_distribution<float> global_dist(0.5f, 99.9f);
                    std::uniform_int_distribution<uint32_t> time_dist(0, 5u * 24u * 3600u); // up to 5 days ago
                    const uint32_t now_ts = (uint32_t)std::time(nullptr);

                    for (auto &ax : achievements) {
                        if (ax.hidden) continue; // skip hidden achievements

                        uint32_t roll = percent_dist(rng);
                        if (ax.max_progress > 0) {
                            // progress achievement: ~40% chance fully achieved, ~40% partial, ~20% zero
                            if (roll < 40) {
                                ax.achieved = true;
                                ax.progress = ax.max_progress;
                                ax.unlock_time = now_ts - time_dist(rng);
                            } else if (roll < 80) {
                                ax.achieved = false;
                                std::uniform_int_distribution<uint32_t> prog_dist(1, ax.max_progress - 1);
                                ax.progress = (ax.max_progress > 1) ? prog_dist(rng) : 0;
                                ax.unlock_time = 0;
                            } else {
                                ax.achieved = false;
                                ax.progress = 0;
                                ax.unlock_time = 0;
                            }
                        } else {
                            // regular achievement: ~40% achieved, ~30% in-progress (fake), ~30% locked
                            if (roll < 40) {
                                ax.achieved = true;
                                ax.unlock_time = now_ts - time_dist(rng);
                                ax.max_progress = 0;
                                ax.progress = 0;
                            } else if (roll < 70) {
                                ax.achieved = false;
                                ax.unlock_time = 0;
                                // assign a fake progress bar so it shows up in "In Progress" tab
                                std::uniform_int_distribution<uint32_t> fake_max(5, 50);
                                ax.max_progress = fake_max(rng);
                                std::uniform_int_distribution<uint32_t> fake_prog(1, ax.max_progress - 1);
                                ax.progress = fake_prog(rng);
                            } else {
                                ax.achieved = false;
                                ax.unlock_time = 0;
                                ax.max_progress = 0;
                                ax.progress = 0;
                            }
                        }

                        // simulate a global percentage only if we don't have a real one
                        if (ach_global_percentages.find(ax.name) == ach_global_percentages.end())
                            ach_global_percentages[ax.name] = global_dist(rng);
                    }
                }

                // ---- Tab bar: In Progress | My Achievements | [Groups] | Global Stats ----
                Steam_User_Stats *steamUserStats_sh = get_steam_client()->steam_user_stats;
                bool has_sh_groups = steamUserStats_sh->steamhunters_data_populated
                                     && !steamUserStats_sh->steamhunters_achievement_groups.empty();
                if (ImGui::BeginTabBar("##ach_tabs")) {
                    if (ImGui::BeginTabItem("In Progress##ach_tab0")) {
                        ach_current_tab = 0;
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("My Achievements##ach_tab1")) {
                        ach_current_tab = 1;
                        ImGui::EndTabItem();
                    }
                    if (has_sh_groups) {
                        if (ImGui::BeginTabItem("Groups##ach_tab2")) {
                            ach_current_tab = 2;
                            ImGui::EndTabItem();
                        }
                    }
                    if (ImGui::BeginTabItem("Global Stats##ach_tab3")) {
                        ach_current_tab = 3;
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                // safety: if groups disappeared while on Groups tab, fall back
                if (ach_current_tab == 2 && !has_sh_groups) ach_current_tab = 1;

                // ---- Search + sort controls ----
                // measure button width to size the search box dynamically
                const auto &style = ImGui::GetStyle();
                float btn_w = 0.0f;
                float spacing = style.ItemSpacing.x;
                const char *sort_labels[] = { "Sort: Global %%##ach_srt", "Sort: Schema Order##ach_srt", "Sort: A-Z##ach_srt" };
                const char *srt_lbl = sort_labels[ach_sort_mode % 3];
                btn_w += ImGui::CalcTextSize(srt_lbl).x + style.FramePadding.x * 2.0f + spacing;
                float avail = ImGui::GetContentRegionAvail().x;
                float search_w = avail - btn_w;
                if (search_w < ImGui::GetFontSize() * 6.0f) search_w = ImGui::GetFontSize() * 6.0f;

                ImGui::SetNextItemWidth(search_w);
                ImGui::InputTextWithHint("##ach_search", "Search achievements...", ach_search_buf, sizeof(ach_search_buf));

                ImGui::SameLine();
                if (ImGui::Button(sort_labels[ach_sort_mode % 3]))
                    ach_sort_mode = (ach_sort_mode + 1) % 3;

                ImGui::Separator();

                ImGui::BeginChild(translationAchievements[current_language]);

                // ---- search filter (case-insensitive substring) ----
                std::string search_lower;
                bool has_search = ach_search_buf[0] != '\0';
                if (has_search) {
                    search_lower = ach_search_buf;
                    std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(),
                        [](unsigned char c){ return std::tolower(c); });
                }
                auto ach_matches_search = [&](const Overlay_Achievement &a) -> bool {
                    if (!has_search) return true;
                    std::string t = a.title;
                    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c){ return std::tolower(c); });
                    if (t.find(search_lower) != std::string::npos) return true;
                    std::string n = a.name;
                    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return std::tolower(c); });
                    if (n.find(search_lower) != std::string::npos) return true;
                    std::string d = a.description;
                    std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c){ return std::tolower(c); });
                    return d.find(search_lower) != std::string::npos;
                };

                // ---- tab filter ----
                auto ach_matches_tab = [&](const Overlay_Achievement &a) -> bool {
                    switch (ach_current_tab) {
                        case 0: // In Progress: has progress tracking and partial progress, not achieved
                            return !a.achieved && a.max_progress > 0 && a.progress > 0;
                        case 1: // My Achievements: all
                            return true;
                        case 2: // Global Stats: all
                            return true;
                        default: return true;
                    }
                };

                // ---- obtainability info from SteamHunters ----
                bool has_sh_data = steamUserStats_sh->steamhunters_data_populated
                                   && !steamUserStats_sh->steamhunters_achievement_data.empty();

                // ---- helper: get best global % (steam → SH local → -1) ----
                auto get_ach_pct = [&](const Overlay_Achievement &a) -> float {
                    auto it = ach_global_percentages.find(a.name);
                    if (it != ach_global_percentages.end() && it->second >= 0.0f) return it->second;
                    if (has_sh_data) {
                        auto sit = steamUserStats_sh->steamhunters_achievement_data.find(a.name);
                        if (sit != steamUserStats_sh->steamhunters_achievement_data.end() && sit->second.localPercentage >= 0.0f)
                            return sit->second.localPercentage;
                    }
                    return -1.0f;
                };

                // ---- comparator (unified across all tabs) ----
                auto ach_compare = [&](size_t ai, size_t bi) -> bool {
                    const auto &a = achievements[ai];
                    const auto &b = achievements[bi];
                    // hidden (locked) achievements last, except Global Stats tab
                    if (ach_current_tab != 3) {
                        bool a_hidden = a.hidden && !a.achieved;
                        bool b_hidden = b.hidden && !b.achieved;
                        if (a_hidden != b_hidden) return !a_hidden;
                        if (a_hidden) return false;
                    }
                    // My Achievements / Groups: achieved first by unlock time
                    if (ach_current_tab == 1 || ach_current_tab == 2) {
                        if (a.achieved != b.achieved) return a.achieved > b.achieved;
                        if (a.achieved) return a.unlock_time > b.unlock_time;
                    }
                    // sort mode
                    if (ach_sort_mode == 0) {
                        float pa = get_ach_pct(a), pb = get_ach_pct(b);
                        if (pa != pb) return pa > pb;
                    } else if (ach_sort_mode == 2) {
                        int cmp = a.title.compare(b.title);
                        if (cmp != 0) return cmp < 0;
                    }
                    return ai < bi; // tie-break: schema order
                };

                // ---- per-achievement render lambda ----
                auto render_ach_item = [&](Overlay_Achievement &x) {
                    bool achieved = x.achieved;
                    bool hidden = x.hidden && !achieved;

                    try_load_ach_icon(x, true,  settings->paginated_achievements_icons == 0);
                    try_load_ach_icon(x, false, settings->paginated_achievements_icons == 0);

                    ImGui::Separator();

                    const float icon_col_w = settings->overlay_appearance.icon_size;
                    const float bar_h      = settings->overlay_appearance.font_size;
                    bool has_icon = x.icon->GetResourceId() != 0 || x.icon_gray->GetResourceId() != 0;
                    bool rendered = false;

                    // Title line with obtainability badges
                    if (has_icon) {
                        const char *sym_for_measure = achieved ? u8"\u2713" : u8"\u2717";
                        float sym_w = ImGui::CalcTextSize(sym_for_measure).x;
                        float title_x_offset = (icon_col_w - sym_w) * 0.5f;
                        if (title_x_offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + title_x_offset);
                    }
                    if (hidden) ImGui::Text("%s", translationHiddenAchievement[current_language]);
                    else        ImGui::Text("%s", x.title.c_str());

                    // Obtainability badges (SteamHunters)
                    if (has_sh_data) {
                        auto sit = steamUserStats_sh->steamhunters_achievement_data.find(x.name);
                        if (sit != steamUserStats_sh->steamhunters_achievement_data.end() && sit->second.obtainability > 0) {
                            ImGui::SameLine();
                            switch (sit->second.obtainability) {
                                case 1: ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "[Missable]"); break;
                                case 2: ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[Bugged]"); break;
                                case 3: ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "[Online Only]"); break;
                                default: ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[Special]"); break;
                            }
                        }
                    }

                    if (has_icon) {
                        std::string tbl_id = std::string("##ach_") + x.name;
                        if (ImGui::BeginTable(tbl_id.c_str(), 2)) {
                            rendered = true;
                            ImGui::TableSetupColumn("img", ImGuiTableColumnFlags_WidthFixed, icon_col_w);
                            ImGui::TableSetupColumn("txt");
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, icon_col_w);
                            ImGui::TableSetColumnIndex(0);
                            auto &icon_rsrc = (achieved && !hidden) ? x.icon : x.icon_gray;
                            if (icon_rsrc->GetResourceId() != 0)
                                ImGui::Image(icon_rsrc->GetResourceId(), ImVec2(icon_col_w, icon_col_w));
                            ImGui::TableSetColumnIndex(1);
                            if (!hidden) ImGui::TextWrapped("%s", x.description.c_str());
                            else         ImGui::TextDisabled("%s", x.description.c_str());
                            ImGui::EndTable();
                        }
                    }

                    if (!rendered) {
                        if (hidden) {
                            ImGui::TextDisabled("%s", x.description.c_str());
                        } else {
                            ImGui::TextWrapped("%s", x.description.c_str());
                        }
                    }

                    // --- status bar ---
                    {
                        const char *sym;
                        ImU32 sym_col;
                        bool has_progress = !achieved && x.max_progress > 0;
                        if (achieved) {
                            sym = u8"\u2713"; sym_col = IM_COL32(0, 220, 0, 255);
                        } else if (has_progress && x.progress > 0) {
                            sym = u8"\u25B6"; sym_col = IM_COL32(255, 180, 0, 255);
                        } else {
                            sym = u8"\u2717"; sym_col = IM_COL32(220, 0, 0, 255);
                        }

                        char date_buf[128]{};
                        if (achieved) {
                            char tmp[80]{};
                            time_t unlock_time = (time_t)x.unlock_time;
                            size_t written = std::strftime(tmp, sizeof(tmp), settings->overlay_appearance.ach_unlock_datetime_format.c_str(), std::localtime(&unlock_time));
                            if (!written) std::strftime(tmp, sizeof(tmp), "%Y/%m/%d - %H:%M:%S", std::localtime(&unlock_time));
                            snprintf(date_buf, sizeof(date_buf), "%s", tmp);
                        }

                        char pbuf[32]{};
                        bool show_progress = x.max_progress > 1 || (x.max_progress > 0 && !achieved);
                        if (show_progress) snprintf(pbuf, sizeof(pbuf), "%u/%u", achieved ? x.max_progress : x.progress, x.max_progress);

                        float fill = achieved ? 1.0f : (has_progress ? (float)x.progress / (float)x.max_progress : 0.0f);
                        ImVec2 bar_pos   = ImGui::GetCursorScreenPos();
                        float  bar_width = ImGui::GetContentRegionAvail().x;
                        ImGui::ProgressBar(fill, ImVec2(-1.0f, bar_h), "");
                        auto  *dl  = ImGui::GetWindowDrawList();
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

                        ImVec2 sym_sz  = fnt->CalcTextSizeA(sym_font_sz, FLT_MAX, 0.0f, sym);
                        ImVec2 sym_pos = { bar_pos.x + 4.0f, bar_pos.y + (bar_h - sym_sz.y) * 0.5f };
                        draw_shadowed_ex(fnt, sym_font_sz, sym_pos, sym_col, sym);

                        if (achieved && date_buf[0]) {
                            float  date_x   = sym_pos.x + sym_sz.x + 4.0f;
                            ImVec2 date_sz  = ImGui::CalcTextSize(date_buf);
                            ImVec2 date_pos = { date_x, bar_pos.y + (bar_h - date_sz.y) * 0.5f };
                            draw_shadowed(date_pos, IM_COL32(255, 255, 255, 255), date_buf);
                        }

                        if (show_progress && pbuf[0]) {
                            ImVec2 pbar_sz  = ImGui::CalcTextSize(pbuf);
                            ImVec2 pbar_pos = { bar_pos.x + (bar_width - pbar_sz.x) * 0.5f, bar_pos.y + (bar_h - pbar_sz.y) * 0.5f };
                            draw_shadowed(pbar_pos, IM_COL32(255, 255, 255, 255), pbuf);
                        }
                    }

                    // --- global % + SteamHunters community % ---
                    {
                        auto it = ach_global_percentages.find(x.name);
                        if (it != ach_global_percentages.end()) {
                            ImGui::TextDisabled(translationGlobalAchievementPercent[current_language], it->second);
                            // Show SteamHunters community % alongside if available
                            if (has_sh_data) {
                                auto sit = steamUserStats_sh->steamhunters_achievement_data.find(x.name);
                                if (sit != steamUserStats_sh->steamhunters_achievement_data.end() && sit->second.localPercentage >= 0.0f) {
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("| %.1f%% of hunters", sit->second.localPercentage);
                                }
                            }
                        }
                    }

                    ImGui::Separator();
                }; // end render_ach_item

                // ---- build name → index map (needed for grouping) ----
                std::unordered_map<std::string, size_t> ach_name_idx;
                ach_name_idx.reserve(achievements.size());
                for (size_t i = 0; i < achievements.size(); ++i)
                    ach_name_idx[achievements[i].name] = i;

                // ---- build filtered index list (tab + search) ----
                std::vector<size_t> filtered_idx;
                filtered_idx.reserve(achievements.size());
                for (size_t i = 0; i < achievements.size(); ++i) {
                    if (!ach_matches_tab(achievements[i])) continue;
                    if (!ach_matches_search(achievements[i])) continue;
                    filtered_idx.push_back(i);
                }

                // ---- "My Achievements" tab: split into Unlocked/Locked sections ----
                if (ach_current_tab == 1) {
                    // Unlocked section
                    std::vector<size_t> unlocked, locked;
                    for (size_t fi : filtered_idx) {
                        if (achievements[fi].achieved) unlocked.push_back(fi);
                        else locked.push_back(fi);
                    }
                    std::stable_sort(unlocked.begin(), unlocked.end(), ach_compare);
                    std::stable_sort(locked.begin(), locked.end(), ach_compare);

                    char hdr_u[64]; snprintf(hdr_u, sizeof(hdr_u), "Unlocked (%d)##ach_unlocked", (int)unlocked.size());
                    char hdr_l[64]; snprintf(hdr_l, sizeof(hdr_l), "Locked (%d)##ach_locked", (int)locked.size());

                    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.15f, 0.35f, 0.15f, 0.80f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.20f, 0.45f, 0.20f, 0.90f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.25f, 0.55f, 0.25f, 1.00f));
                    bool open_u = ImGui::CollapsingHeader(hdr_u, ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::PopStyleColor(3);
                    if (open_u) {
                        for (size_t si : unlocked) render_ach_item(achievements[si]);
                    }

                    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.35f, 0.20f, 0.20f, 0.80f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.45f, 0.25f, 0.25f, 0.90f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.55f, 0.30f, 0.30f, 1.00f));
                    bool open_l = ImGui::CollapsingHeader(hdr_l, ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::PopStyleColor(3);
                    if (open_l) {
                        for (size_t si : locked) render_ach_item(achievements[si]);
                    }
                } else if (ach_current_tab == 2 && has_sh_groups) {
                    // === GROUPED RENDER (Groups tab) ===
                    std::unordered_set<size_t> rendered_set;
                    std::unordered_set<size_t> filtered_set(filtered_idx.begin(), filtered_idx.end());

                    for (const auto &grp : steamUserStats_sh->steamhunters_achievement_groups) {
                        std::vector<size_t> grp_idx;
                        for (const auto &api_name : grp.achievementApiNames) {
                            auto it = ach_name_idx.find(api_name);
                            if (it != ach_name_idx.end() && filtered_set.count(it->second)) {
                                grp_idx.push_back(it->second);
                                rendered_set.insert(it->second);
                            }
                        }
                        if (grp_idx.empty()) continue;
                        std::stable_sort(grp_idx.begin(), grp_idx.end(), ach_compare);

                        // group header: "DLC Name" or "DLC Name — Sub-group"
                        std::string hdr = grp.dlcAppName.empty() ? "Base Game" : grp.dlcAppName;
                        if (!grp.name.empty()) hdr += " \xe2\x80\x94 " + grp.name; // em-dash

                        // count achieved in group
                        int grp_done = 0;
                        for (size_t gi : grp_idx) if (achievements[gi].achieved) ++grp_done;
                        char grp_count[32]; snprintf(grp_count, sizeof(grp_count), " (%d/%d)", grp_done, (int)grp_idx.size());
                        hdr += grp_count;

                        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.20f, 0.30f, 0.45f, 0.80f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.25f, 0.38f, 0.55f, 0.90f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.30f, 0.45f, 0.65f, 1.00f));
                        bool open = ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (size_t gi : grp_idx) render_ach_item(achievements[gi]);
                        }
                    }

                    // render any achievements not assigned to any group as "Other"
                    std::vector<size_t> ungrouped;
                    for (size_t fi : filtered_idx)
                        if (rendered_set.find(fi) == rendered_set.end()) ungrouped.push_back(fi);
                    if (!ungrouped.empty()) {
                        std::stable_sort(ungrouped.begin(), ungrouped.end(), ach_compare);
                        int ug_done = 0;
                        for (size_t gi : ungrouped) if (achievements[gi].achieved) ++ug_done;
                        char ug_hdr[64]; snprintf(ug_hdr, sizeof(ug_hdr), "Base Game (%d/%d)##ach_base", ug_done, (int)ungrouped.size());
                        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.20f, 0.30f, 0.45f, 0.80f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.25f, 0.38f, 0.55f, 0.90f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.30f, 0.45f, 0.65f, 1.00f));
                        bool open = ImGui::CollapsingHeader(ug_hdr, ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (size_t gi : ungrouped) render_ach_item(achievements[gi]);
                        }
                    }
                } else {
                    // === FLAT SORTED RENDER ===
                    std::stable_sort(filtered_idx.begin(), filtered_idx.end(), ach_compare);
                    for (size_t si : filtered_idx) render_ach_item(achievements[si]);
                }

                if (filtered_idx.empty()) {
                    if (ach_current_tab == 0)
                        ImGui::TextDisabled("No achievements with active progress.");
                    else if (has_search)
                        ImGui::TextDisabled("No achievements match your search.");
                }

                ImGui::EndChild();
            }
            
            ImGui::End();
        }

        // SCE asset browser window — ≥50% wide, shown when Browse SCE Assets is toggled on
        if (show_sce_browser) {
            Steam_User_Stats *user_stats = get_steam_client()->steam_user_stats;
            if (!user_stats->sce_data_populated || user_stats->sce_game_data.series.empty()) {
                show_sce_browser = false;
                sce_textures_pending_free = true;  // defer to next frame start
            } else {
                auto local_storage = get_steam_client()->local_storage;

                // Same subfolder mapping as the downloader
                auto type_subfolder = [](int tidx) -> const char * {
                    static constexpr const char *dirs[] = {
                        "cards", "foil_cards", "booster_packs",
                        "badges", "foil_badges", "emoticons",
                        "backgrounds", "animated_backgrounds", "animated_mini_backgrounds",
                        "profiles", "avatar_frames", "animated_avatars",
                        "animated_stickers",
                        "startup_movies"
                    };
                    return (tidx >= 0 && tidx < 14) ? dirs[tidx] : "misc";
                };

                // Same sanitize as the downloader
                auto sanitize = [](std::string s) -> std::string {
                    for (char &c : s) {
                        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
                            c = '_';
                    }
                    return s;
                };

                // Extract filename extension from a URL
                auto url_ext = [](const std::string &url) -> std::string {
                    size_t slash = url.rfind('/');
                    std::string name = (slash != std::string::npos) ? url.substr(slash + 1) : url;
                    size_t q = name.find('?');
                    if (q != std::string::npos) name.resize(q);
                    size_t dot = name.rfind('.');
                    return (dot != std::string::npos) ? name.substr(dot) : ".png";
                };

                // Per-type card dimensions: {card_width, image_height}
                // Mirrors SCE website proportions: portrait for cards, landscape for BGs, etc.
                struct CardDims { float cw, ih; };
                static constexpr CardDims kDims[14] = {
                    {160.f, 200.f},  // 0  cards                 (portrait ~0.80)
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
                    {220.f, 124.f},  // 13 startup_movies         (16:9 landscape)
                };

                // Rarity colours matching SCE site: common=grey, uncommon=green, rare=red
                auto rarity_color = [](const std::string &r) -> ImVec4 {
                    if (r == "Uncommon") return {0.20f, 0.85f, 0.20f, 1.0f};
                    if (r == "Rare")     return {0.90f, 0.20f, 0.20f, 1.0f};
                    return {0.60f, 0.60f, 0.60f, 1.0f};
                };

                constexpr int MAX_TEX_PER_FRAME = 4;
                int tex_loaded_this_frame = 0;
                constexpr float CARD_GAP = 12.0f;

                const float min_w = io.DisplaySize.x * 0.60f;
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(min_w, io.DisplaySize.y * 0.50f),
                    ImVec2(8192.0f, 8192.0f));
                ImGui::SetNextWindowBgAlpha(1.0f);
                bool browser_open = show_sce_browser;
                if (ImGui::Begin("SCE Assets##sce_browser", &browser_open)) {
                    ImGui::TextDisabled("Assets folder: GSE Saves/%u/%s",
                        user_stats->sce_game_data.appid,
                        Steam_User_Stats::sce_assets_folder);
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Tab grouping: multiple asset types share a tab
                    struct SceTabGroup { const char *label; int types[4]; int ntype; };
                    static constexpr SceTabGroup kTabs[] = {
                        { "Cards",       { 0,  1,  2, -1}, 3 },  // Trading Cards + Foil Cards + Booster Packs
                        { "Badges",      { 3,  4, -1, -1}, 2 },  // Badges + Foil Badges
                        { "Backgrounds", { 6,  7,  8, -1}, 3 },  // Backgrounds + Animated + Animated Mini
                        { "Chat",        { 5, 12, -1, -1}, 2 },  // Emoticons + Animated Stickers
                        { "Profiles",    {11, 10,  9, -1}, 3 },  // Animated Avatars + Avatar Frames + Profiles
                        { "Steam",       {13, -1, -1, -1}, 1 },  // Startup Movies (keyboard themes not on SCE)
                    };

                    for (const auto &series : user_stats->sce_game_data.series) {
                        char ser_label[128]{};
                        if (!series.series_name.empty())
                            snprintf(ser_label, sizeof(ser_label),
                                "Series %d - %s", series.series_number, series.series_name.c_str());
                        else
                            snprintf(ser_label, sizeof(ser_label), "Series %d", series.series_number);

                        char ser_num_str[8]{};
                        snprintf(ser_num_str, sizeof(ser_num_str), "%02d", series.series_number);
                        std::string ser_dir = std::string("Series ") + ser_num_str;
                        if (!series.series_name.empty())
                            ser_dir += " - " + sanitize(series.series_name);

                        if (!ImGui::CollapsingHeader(ser_label, ImGuiTreeNodeFlags_DefaultOpen))
                            continue;

                        // Group items by type
                        std::map<int, std::vector<const Steam_User_Stats::SceItem *>> by_type;
                        for (const auto &item : series.items)
                            by_type[(int)item.type].push_back(&item);

                        char tab_bar_id[32]{};
                        snprintf(tab_bar_id, sizeof(tab_bar_id), "##tb_%d", series.series_number);
                        if (!ImGui::BeginTabBar(tab_bar_id)) { ImGui::Spacing(); continue; }

                        for (int tgi = 0; tgi < (int)(sizeof(kTabs)/sizeof(kTabs[0])); ++tgi) {
                            const auto &tg = kTabs[tgi];

                            // Count total items for this tab group in this series
                            int tab_total = 0;
                            for (int ti = 0; ti < tg.ntype; ++ti) {
                                auto it2 = by_type.find(tg.types[ti]);
                                if (it2 != by_type.end()) tab_total += (int)it2->second.size();
                            }
                            if (tab_total == 0) continue;

                            char tab_lbl[80]{};
                            snprintf(tab_lbl, sizeof(tab_lbl), "%s (%d)##tb_%d_%d",
                                tg.label, tab_total, series.series_number, tgi);
                            if (!ImGui::BeginTabItem(tab_lbl)) continue;

                            bool first_type = true;
                            for (int ti = 0; ti < tg.ntype; ++ti) {
                                int tidx = tg.types[ti];
                                auto it = by_type.find(tidx);
                                if (it == by_type.end() || it->second.empty()) continue;

                                const auto &items_vec = it->second;
                                float card_w = kDims[tidx].cw;
                                float img_h  = kDims[tidx].ih;

                                // Sub-header per type
                                {
                                    if (!first_type) ImGui::Spacing();
                                    ImGui::TextDisabled("%s  (%zu)",
                                        Steam_User_Stats::SCE_TYPE_LABELS[tidx], items_vec.size());
                                    ImGui::Separator();
                                    ImGui::Spacing();
                                }
                                first_type = false;

                                // Wrapping card grid
                                float avail_w = ImGui::GetContentRegionAvail().x;
                                int cols = std::max(1, (int)((avail_w + CARD_GAP) / (card_w + CARD_GAP)));
                                char tbl_id[32]{};
                                snprintf(tbl_id, sizeof(tbl_id), "##cg_%d_%d", series.series_number, tidx);
                                if (ImGui::BeginTable(tbl_id, cols,
                                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody |
                                        ImGuiTableFlags_NoPadOuterX)) {
                                    for (int c = 0; c < cols; ++c)
                                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, card_w);

                                    int slot_idx = 0;
                                    for (const auto *item : items_vec) {
                                        ++slot_idx;
                                        ImGui::TableNextColumn();

                                        // Is this a (animated) background? Use dedicated thumb file.
                                        bool is_bg = (tidx == 6 || tidx == 7 || tidx == 8);

                                        // Build thumbnail file path (mirrors downloader naming)
                                        const std::string &thumb_src = is_bg
                                            ? (item->wallpaper_url.empty() ? item->static_img_url : item->wallpaper_url)
                                            : (!item->icon_url.empty() ? item->icon_url : item->static_img_url);
                                        std::string ext = url_ext(thumb_src);
                                        std::string item_label = sanitize(item->name);
                                        if (item_label.size() > 48) item_label.resize(48);
                                        char prefix[8]{};
                                        snprintf(prefix, sizeof(prefix), "%02d_", slot_idx);
                                        // Backgrounds: "01_thumb_wallpaper_Name.jpg"; others: "01_icon_Name.ext"
                                        std::string filename = is_bg
                                            ? (std::string(prefix) + "thumb_wallpaper_" + item_label + ext)
                                            : (std::string(prefix) + "icon_" + item_label + ext);
                                        std::string type_dir = type_subfolder(tidx);
                                        std::string folder = std::string(Steam_User_Stats::sce_assets_folder)
                                            + PATH_SEPARATOR + ser_dir
                                            + PATH_SEPARATOR + type_dir;
                                        std::string tex_key = folder + PATH_SEPARATOR + filename;

                                        bool is_static = (ext != ".gif" && ext != ".mp4" && ext != ".webm");
                                        auto &tex = sce_textures[tex_key];
                                        if (is_static && !tex.load_attempted
                                                && tex_loaded_this_frame < MAX_TEX_PER_FRAME) {
                                            tex.load_attempted = true;
                                            int pw = 0, ph = 0;
                                            tex.pixels = local_storage->load_image_from_folder(folder, filename, pw, ph);
                                            if (!tex.pixels.empty() && _renderer && pw > 0 && ph > 0) {
                                                srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
                                                    (uint8_t*)tex.pixels.data(), (size_t)pw * ph);
                                                tex.resource = _renderer->CreateResource();
                                                tex.w = pw; tex.h = ph;
                                                tex.resource->AttachResource(tex.pixels.data(), (uint32_t)pw, (uint32_t)ph);
                                            }
                                            ++tex_loaded_this_frame;
                                        }

                                        // --- Card ---
                                        ImGui::BeginGroup();

                                        // Slot / total line above image
                                        if (item->slot > 0 && item->total > 0)
                                            ImGui::TextDisabled("#%d of %d", item->slot, item->total);
                                        else if (item->slot > 0)
                                            ImGui::TextDisabled("#%d", item->slot);
                                        else
                                            ImGui::TextDisabled(" ");

                                        // Image area
                                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                                        ImVec2 p1 = ImVec2(p0.x + card_w, p0.y + img_h);
                                        ImDrawList *dl = ImGui::GetWindowDrawList();
                                        dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 50, 255));

                                        char btn_id[64]{};
                                        snprintf(btn_id, sizeof(btn_id), "##bg_%d_%d_%d", series.series_number, tidx, slot_idx);
                                        ImGui::InvisibleButton(btn_id, ImVec2(card_w, img_h));
                                        bool clicked = ImGui::IsItemClicked();

                                        if (is_static && tex.resource
                                                && tex.resource->GetResourceId() != 0) {
                                            // Aspect-fit (letterbox) image into card_w × img_h
                                            float sa = (tex.h > 0) ? (float)tex.w / tex.h : 1.0f;
                                            float da = card_w / img_h;
                                            float dw, dh, ox = 0.f, oy = 0.f;
                                            if (sa >= da) {
                                                dw = card_w; dh = card_w / sa;
                                                oy = (img_h - dh) * 0.5f;
                                            } else {
                                                dh = img_h; dw = img_h * sa;
                                                ox = (card_w - dw) * 0.5f;
                                            }
                                            dl->AddImage(tex.resource->GetResourceId(),
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

                                        // On click: open full-size preview.
                                        // Animated BGs (types 7/8) use wallpaper_ file; all others use icon_.
                                        bool is_animated_bg = (tidx == 7 || tidx == 8);
                                        bool can_preview = is_animated_bg ? !item->wallpaper_url.empty() : is_static;
                                        if (clicked && can_preview) {
                                            if (is_animated_bg) {
                                                std::string full_ext = url_ext(item->wallpaper_url);
                                                std::string full_filename = std::string(prefix) + "wallpaper_" + item_label + full_ext;
                                                sce_preview_key = folder + PATH_SEPARATOR + full_filename;
                                            } else {
                                                sce_preview_key = tex_key;
                                            }
                                            // Build nav keys: same type within same series
                                            sce_preview_nav_keys.clear();
                                            for (int ni = 0; ni < (int)items_vec.size(); ++ni) {
                                                const auto *nitem = items_vec[ni];
                                                int nslot2 = ni + 1;
                                                char npfx[8]{}; snprintf(npfx, sizeof(npfx), "%02d_", nslot2);
                                                std::string nlbl = sanitize(nitem->name);
                                                if (nlbl.size() > 48) nlbl.resize(48);
                                                std::string npreview_file;
                                                if (is_animated_bg) {
                                                    if (nitem->wallpaper_url.empty()) continue;
                                                    std::string next = url_ext(nitem->wallpaper_url);
                                                    npreview_file = std::string(npfx) + "wallpaper_" + nlbl + next;
                                                } else {
                                                    const std::string &nsrc = !nitem->icon_url.empty() ? nitem->icon_url : nitem->static_img_url;
                                                    std::string next = url_ext(nsrc);
                                                    npreview_file = std::string(npfx) + "icon_" + nlbl + next;
                                                }
                                                sce_preview_nav_keys.push_back(folder + PATH_SEPARATOR + npreview_file);
                                            }
                                        }

                                        // Name (wrapped to card width)
                                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w);
                                        ImGui::TextUnformatted(item->name.c_str());
                                        ImGui::PopTextWrapPos();

                                        // Rarity (coloured)
                                        if (!item->rarity.empty() && item->rarity != "Unknown")
                                            ImGui::TextColored(rarity_color(item->rarity),
                                                "%s", item->rarity.c_str());
                                        else
                                            ImGui::TextDisabled(" ");

                                        // Price
                                        if (!item->price_text.empty())
                                            ImGui::TextDisabled("%s", item->price_text.c_str());
                                        else
                                            ImGui::TextDisabled(" ");

                                        // Badge level / XP (types 3 + 4)
                                        if ((tidx == 3 || tidx == 4) && (item->badge_level > 0 || item->badge_xp > 0)) {
                                            if (item->badge_level > 0 && item->badge_xp > 0)
                                                ImGui::TextDisabled("Lv.%d  %d XP", item->badge_level, item->badge_xp);
                                            else if (item->badge_level > 0)
                                                ImGui::TextDisabled("Lv.%d", item->badge_level);
                                            else
                                                ImGui::TextDisabled("%d XP", item->badge_xp);
                                        }

                                        // Emoticon shortcode (type 5)
                                        if (tidx == 5 && !item->emoticon_name.empty())
                                            ImGui::TextDisabled(":%s:", item->emoticon_name.c_str());

                                        // Steam Points cost (profiles, avatar frames, animated avatars, stickers)
                                        if (!item->points_price.empty())
                                            ImGui::TextDisabled("Pts: %s", item->points_price.c_str());

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

                // Full-size asset preview — standalone window
                if (!sce_preview_key.empty()) {
                    // Dim everything behind the preview window
                    ImGui::GetBackgroundDrawList()->AddRectFilled(
                        ImVec2(0, 0), io.DisplaySize,
                        IM_COL32(0, 0, 0, 180));

                    auto &ftex = sce_textures[sce_preview_key];
                    if (!ftex.load_attempted) {
                        // User explicitly opened this — bypass the per-frame cap
                        ftex.load_attempted = true;
                        size_t sep = sce_preview_key.rfind(PATH_SEPARATOR[0]);
                        if (sep != std::string::npos) {
                            std::string f_folder = sce_preview_key.substr(0, sep);
                            std::string f_file   = sce_preview_key.substr(sep + 1);
                            int pw = 0, ph = 0;
                            ftex.pixels = local_storage->load_image_from_folder(f_folder, f_file, pw, ph);
                            if (!ftex.pixels.empty() && _renderer && pw > 0 && ph > 0) {
                                srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
                                    (uint8_t*)ftex.pixels.data(), (size_t)pw * ph);
                                ftex.resource = _renderer->CreateResource();
                                ftex.w = pw; ftex.h = ph;
                                ftex.resource->AttachResource(ftex.pixels.data(), (uint32_t)pw, (uint32_t)ph);
                            }
                        }
                    }

                    float pw = io.DisplaySize.x * 0.75f;
                    float ph = pw * (9.f / 16.f); // default 16:9
                    if (ftex.w > 0 && ftex.h > 0) {
                        ph = pw * ((float)ftex.h / ftex.w);
                        float max_h = io.DisplaySize.y * 0.85f;
                        if (ph > max_h) { ph = max_h; pw = ph * ((float)ftex.w / ftex.h); }
                    }
                    ImVec2 wpos(
                        (io.DisplaySize.x - pw) * 0.5f,
                        (io.DisplaySize.y - ph) * 0.5f);
                    ImGui::SetNextWindowPos(wpos, ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.97f);
                    bool preview_open = true;
                    if (ImGui::Begin("##sce_preview", &preview_open,
                            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
                        constexpr float BTN_H = 28.f;
                        constexpr float BTN_W = 64.f;
                        constexpr float NAV_PAD = 8.f; // padding above/below nav bar
                        float img_avail_h = ImGui::GetContentRegionAvail().y - BTN_H - NAV_PAD * 2.f;
                        if (ftex.resource && ftex.resource->GetResourceId() != 0) {
                            ImVec2 avail = ImVec2(ImGui::GetContentRegionAvail().x, img_avail_h);
                            float sa = (ftex.h > 0) ? (float)ftex.w / ftex.h : 1.0f;
                            float dw = avail.x, dh = avail.x / sa;
                            if (dh > avail.y) { dh = avail.y; dw = avail.y * sa; }
                            float padx = (avail.x - dw) * 0.5f;
                            float pady = (avail.y - dh) * 0.5f;
                            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + padx,
                                                       ImGui::GetCursorPosY() + pady));
                            ImGui::Image(ftex.resource->GetResourceId(), ImVec2(dw, dh));
                        } else {
                            ImGui::SetCursorPosY(img_avail_h * 0.45f);
                            ImGui::SetCursorPosX((pw - ImGui::CalcTextSize("Loading...").x) * 0.5f);
                            ImGui::TextDisabled("Loading...");
                        }
                        // --- Navigation buttons ---
                        int nav_cur = -1;
                        for (int ni = 0; ni < (int)sce_preview_nav_keys.size(); ++ni)
                            if (sce_preview_nav_keys[ni] == sce_preview_key) { nav_cur = ni; break; }

                        auto nav_to = [&](int ni) {
                            sce_preview_key = sce_preview_nav_keys[ni];
                        };

                        bool can_prev = nav_cur > 0;
                        bool can_next = nav_cur >= 0 && nav_cur < (int)sce_preview_nav_keys.size() - 1;

                        // Arrow / A-D key navigation (NoNav is set but IsKeyPressed still works)
                        if (can_prev && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  || ImGui::IsKeyPressed(ImGuiKey_A))) nav_to(nav_cur - 1);
                        if (can_next && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D))) nav_to(nav_cur + 1);

                        // Button bar pinned to bottom of window via screen coords
                        ImVec2 cpos = ImGui::GetWindowPos();
                        ImVec2 csz  = ImGui::GetWindowSize();
                        float  by   = cpos.y + csz.y - BTN_H - 6.f;

                        ImGui::SetCursorScreenPos(ImVec2(cpos.x + 6.f, by));
                        ImGui::BeginDisabled(!can_prev);
                        if (ImGui::Button("< Prev", ImVec2(BTN_W, BTN_H)) && can_prev) nav_to(nav_cur - 1);
                        ImGui::EndDisabled();

                        // Counter label centred
                        if (nav_cur >= 0) {
                            char cnt[32]{};
                            snprintf(cnt, sizeof(cnt), "%d / %d",
                                nav_cur + 1, (int)sce_preview_nav_keys.size());
                            ImVec2 tsz = ImGui::CalcTextSize(cnt);
                            ImGui::SetCursorScreenPos(ImVec2(
                                cpos.x + (csz.x - tsz.x) * 0.5f,
                                by + (BTN_H - tsz.y) * 0.5f));
                            ImGui::TextDisabled("%s", cnt);
                        }

                        ImGui::SetCursorScreenPos(ImVec2(cpos.x + csz.x - BTN_W - 6.f, by));
                        ImGui::BeginDisabled(!can_next);
                        if (ImGui::Button("Next >", ImVec2(BTN_W, BTN_H)) && can_next) nav_to(nav_cur + 1);
                        ImGui::EndDisabled();

                        if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                            ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                            (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                              ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                            preview_open = false;
                    }
                    ImGui::End();
                    if (!preview_open)
                        sce_preview_key.clear();
                }

                // Free textures when the window is closed (deferred to avoid DX9 crash)
                if (!browser_open) {
                    show_sce_browser = false;
                    sce_preview_key.clear();
                    sce_preview_nav_keys.clear();
                    sce_textures_pending_free = true;  // defer to next frame start
                }
            }
        }

        // user clicked on "settings" button
        if (show_settings) {
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationGlobalSettingsWindow[current_language], &show_settings)) {
                ImGui::Text("%s", translationGlobalSettingsWindowDescription[current_language]);

                ImGui::Separator();

                ImGui::Text("%s", translationUsername[current_language]);
                ImGui::SameLine();
                ImGui::InputText("##username", username_text, sizeof(username_text), 0);

                ImGui::Separator();

                ImGui::Text("%s", translationLanguage[current_language]);
                ImGui::ListBox("##language", &current_language, valid_languages, sizeof(valid_languages) / sizeof(valid_languages[0]), 7);
                ImGui::Text(translationSelectedLanguage[current_language], valid_languages[current_language]);

                ImGui::Separator();

                ImGui::Text("%s", translationRestartTheGameToApply[current_language]);
                if (ImGui::Button(translationSave[current_language])) {
                    save_settings = true;
                    show_settings = false;
                }
            }

            ImGui::End();
        }

        // Networks panel — show all adapters and users grouped by subnet
        if (show_networks) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20, ImGui::GetFontSize() * 12), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin("Networks", &show_networks)) {
                GSE_NetAdapter adapters[8];
                int count = Bridge_GetNetworkInfo(adapters, 8);

                if (count == 0) {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No network adapters detected");
                }

                for (int ai = 0; ai < count; ++ai) {
                    auto &a = adapters[ai];
                    // Collapsible header per adapter
                    char header[256];
                    if (a.subnet_str[0])
                        snprintf(header, sizeof(header), "%s (%s) - %s", a.name, a.ip_str, a.subnet_str);
                    else
                        snprintf(header, sizeof(header), "%s", a.name);

                    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (a.range_str[0]) {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  Range: %s", a.range_str);
                        }
                        for (int ui = 0; ui < a.user_count; ++ui) {
                            auto &u = a.users[ui];
                            ImGui::PushID(ai * 100 + ui);

                            if (u.is_self) {
                                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), u8"  \u25CF %s", u.name);
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "(%s)", u.ip_str);
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[You]");
                            } else {
                                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), u8"  \u25CF %s", u.name);
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "(%s)", u.ip_str);
                            }
                            // Right-click context menu for copy
                            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                                ImGui::OpenPopup("##net_ctx");
                            if (ImGui::BeginPopup("##net_ctx")) {
                                if (ImGui::MenuItem("Copy IP")) {
                                    ImGui::SetClipboardText(u.ip_str);
                                }
                                if (ImGui::MenuItem("Copy Name")) {
                                    ImGui::SetClipboardText(u.name);
                                }
                                char id_str[32];
                                snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)u.steam_id);
                                if (ImGui::MenuItem("Copy Steam ID")) {
                                    ImGui::SetClipboardText(id_str);
                                }
                                ImGui::EndPopup();
                            }

                            ImGui::PopID();
                        }
                        if (a.user_count == 0) {
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  (no users detected)");
                        }
                    }
                }
            }
            ImGui::End();
        }

        // Lobby Chat window
        if (show_lobby_chat && i_have_lobby) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 16), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin("Lobby Chat", &show_lobby_chat)) {
                GSE_LobbyChatState lcs{};
                bool got_state = Bridge_GetLobbyChatState(&lcs) != 0;

                if (got_state) {
                    // Header: member list
                    ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "Members (%d):", lcs.member_count);
                    ImGui::SameLine();
                    for (int i = 0; i < lcs.member_count; ++i) {
                        if (i > 0) ImGui::SameLine();
                        bool is_self = (lcs.members[i].steam_id == settings->get_local_steam_id().ConvertToUint64());
                        if (is_self)
                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", lcs.members[i].name);
                        else
                            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "%s", lcs.members[i].name);
                        if (i < lcs.member_count - 1) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ",");
                        }
                    }
                    ImGui::Separator();

                    // Chat history area
                    float footer_height = ImGui::GetFrameHeightWithSpacing() + 4;
                    ImGui::BeginChild("##lobby_chat_history", ImVec2(0, -footer_height), true);

                    if (lcs.chat_history[0]) {
                        char *history = lcs.chat_history;
                        char *line = history;
                        while (*line) {
                            char *end = strchr(line, '\n');
                            if (end) *end = '\0';

                            bool is_self = (strncmp(line, "You: ", 5) == 0);
                            if (is_self)
                                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", line);
                            else
                                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "%s", line);

                            if (!end) break;
                            *end = '\n';
                            line = end + 1;
                        }
                    } else {
                        ImGui::TextDisabled("No messages yet.");
                    }

                    // Auto-scroll to bottom when new messages arrive
                    static size_t last_history_len = 0;
                    size_t cur_history_len = lcs.history_len;
                    if (cur_history_len != last_history_len) {
                        ImGui::SetScrollHereY(1.0f);
                        last_history_len = cur_history_len;
                    }

                    ImGui::EndChild();

                    // Input bar
                    float send_btn_w = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2 + 8;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - send_btn_w - 8);

                    bool send_msg = false;
                    if (ImGui::InputText("##lobby_chat_input", lobby_chat_input, sizeof(lobby_chat_input),
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
                        send_msg = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Send##lobby_send") || send_msg) {
                        if (lobby_chat_input[0]) {
                            Bridge_SendLobbyChatMsg(lobby_chat_input);
                            lobby_chat_input[0] = '\0';
                        }
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Not in a lobby.");
                }
            }
            ImGui::End();
        } else if (!i_have_lobby) {
            show_lobby_chat = false;
        }

        // we have a url to open/display
        if (show_url.size()) {
            std::string url = show_url;
            bool show = true;
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(URL_WINDOW_NAME, &show)) {
                ImGui::Text("%s", translationSteamOverlayURL[current_language]);
                ImGui::Spacing();

                ImGui::PushItemWidth(ImGui::CalcTextSize(url.c_str()).x + 20);
                ImGui::InputText("##url_copy", (char *)url.data(), url.size(), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopItemWidth();
                
                ImGui::Spacing();
                if (ImGui::Button(translationClose[current_language]) || !show)
                    show_url = "";
                // ImGui::SetWindowSize(ImVec2(ImGui::CalcTextSize(url.c_str()).x + 10, 0));
            }

            ImGui::End();
        }

        bool show_warning = warn_local_save || warn_bad_appid;
        if (show_warning) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 32, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationWarning[current_language], &show_warning)) {
                if (warn_bad_appid) {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription_badAppid[current_language]);
                    ImGui::TextColored(ImVec4(255, 0, 0, 255),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                }
                if (warn_local_save) {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription_localSave[current_language]);
                    ImGui::TextColored(ImVec4(255, 0, 0, 255),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                }
            }

            ImGui::End();
            // if button closed, don't show the warning again
            if (!show_warning) {
                warn_local_save = false;
                warn_bad_appid = false;
            }
        }
    }

    ImGui::End();

    if (style_color_stack) ImGui::PopStyleColor(style_color_stack);
    ImGui::PopFont();

    if (!show) {
        ShowOverlay(false);
    }

}

void Steam_Overlay::load_next_ach_icon()
{
    // this function only works when icons pagination is active, request-based loading is not supported too (pagination=0)
    if (!settings->overlay_upload_achs_icons_to_gpu || settings->paginated_achievements_icons <= 0 || achievements.empty()) return;

    size_t linear_idx = last_loaded_ach_icon / 2; // 2 icons per achievement, 1 achieved, 1 unachieved
    if (linear_idx >= achievements.size()) {
        last_loaded_ach_icon = 0;
        linear_idx = 0;
    }

#ifndef EMU_RELEASE_BUILD
    auto now1 = std::chrono::high_resolution_clock::now();
#endif

    auto &ach = achievements.at(linear_idx);
    ++last_loaded_ach_icon;

    bool achieved = last_loaded_ach_icon % 2 != 0;
    auto &icon_rsrc = achieved ? ach.icon : ach.icon_gray;
    // always force upload to GPU in background-loading mode (pagination > 0)
    bool loaded = try_load_ach_icon(ach, achieved, true);
    
#ifndef EMU_RELEASE_BUILD
    if (loaded) {
        auto now2 = std::chrono::high_resolution_clock::now();
        auto dd = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
        PRINT_DEBUG("uploaded an achievement icon to GPU in %u ms", dd);
    }
#endif

}

void Steam_Overlay::SetupOverlay()
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    bool not_called_yet = false;
    if (setup_overlay_called.compare_exchange_weak(not_called_yet, true)) {
        // In bridge-only mode (overlay disabled but bridge enabled), skip the
        // native renderer detector — the ReShade addon handles all rendering.
        if (settings->disable_overlay) return;

        if (settings->overlay_hook_delay_sec > 0) {
            PRINT_DEBUG("waiting %i seconds", settings->overlay_hook_delay_sec);
            renderer_detector_delay_thread.start();
        } else {
            // "HITMAN 3" fails if the detector was started later (after a delay)
            // so request the renderer detector immediately (the old behavior)
            request_renderer_detector();
            set_renderer_hook_timeout();
            renderer_hook_init_thread.start();
        }
    }
}

void Steam_Overlay::UnSetupOverlay()
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    bool already_called = true;
    if (setup_overlay_called.compare_exchange_weak(already_called, false)) {
        is_ready = false;

        renderer_hook_init_thread.kill();
        renderer_detector_delay_thread.kill();

        // stop internal frame processing & restore cursor
        if (_renderer) {
            // for some reason this gets triggered after the overlay instance has been destroyed
            // I assume because the game de-initializes DX later after closing Steam APIs
            // this hacky solution just sets it to an empty function
            _renderer->OverlayHookReady = [](InGameOverlay::OverlayHookState){};
            _renderer->OverlayProc = [](){};
            
            allow_renderer_frame_processing(false, true);
            obscure_game_input(false);
            
            PRINT_DEBUG("releasing any images resources");
            for (auto &ach : achievements) {
                if (ach.icon->GetResourceId() != 0) {
                    ach.icon->Unload();
                }

                if (ach.icon_gray->GetResourceId() != 0) {
                    ach.icon_gray->Unload();
                }
            }

            // manually calling this dtor looks bad, but it actually prevents a lot of crashes on exit, don't remove it!
            // many DX12 games will crash on exit if the hook wasn't manually removed (ex appid 2933080, 1583230)
            _renderer->~RendererHook_t();
            _renderer = nullptr;
        }

        cleanup_renderer_hook();
    }
    
    PRINT_DEBUG("done *********");
}

bool Steam_Overlay::Ready() const
{
    // Bridge-only mode: Ready if bridge is connected (it sets is_ready + late_init_imgui)
    if (settings->disable_overlay && settings->enable_overlay_bridge)
        return is_ready && late_init_imgui;
    return !settings->disable_overlay && is_ready && late_init_imgui;
}

bool Steam_Overlay::NeedPresent() const
{
    PRINT_DEBUG_ENTRY();
    return !settings->disable_overlay || settings->enable_overlay_bridge;
}

void Steam_Overlay::SetNotificationPosition(ENotificationPosition eNotificationPosition)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("TODO %i", (int)eNotificationPosition);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    notif_position = eNotificationPosition;
}

void Steam_Overlay::SetNotificationInset(int nHorizontalInset, int nVerticalInset)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("TODO x=%i y=%i", nHorizontalInset, nVerticalInset);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    h_inset = nHorizontalInset;
    v_inset = nVerticalInset;
}

void Steam_Overlay::OpenOverlayInvite(CSteamID lobbyId)
{
    PRINT_DEBUG("TODO %llu", lobbyId.ConvertToUint64());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    ShowOverlay(true);
}

void Steam_Overlay::OpenOverlay(const char* pchDialog)
{
    PRINT_DEBUG("TODO '%s'", pchDialog);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    // TODO: Show pages depending on pchDialog
    if ((strncmp(pchDialog, "Friends", sizeof("Friends") - 1) == 0) && (settings->overlayAutoAcceptInvitesCount() > 0)) {
        PRINT_DEBUG("won't open overlay's friends list because some friends are defined in the auto accept list");
        add_auto_accept_invite_notification();
    } else {
        ShowOverlay(true);
    }
}

void Steam_Overlay::OpenOverlayWebpage(const char* pchURL)
{
    PRINT_DEBUG("TODO '%s'", pchURL);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    show_url = pchURL;
    ShowOverlay(true);
}

bool Steam_Overlay::ShowOverlay() const
{
    return show_overlay;
}

void Steam_Overlay::ShowOverlay(bool state)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready() || show_overlay == state) return;

    show_overlay = state;
    overlay_state_changed = true;

    // Refresh display HDR/SDR info and swapchain detection every time the overlay opens.
    // This picks up any SDR white level changes the user may have made in Windows settings
    // (HDR brightness slider) as well as HDR on/off toggles, without requiring a restart.
    if (state) {
        refresh_sdr_white_scale();
        if (_renderer)
            arm_swapchain_format_detect(_renderer);
    }

    // On close: free GPU-side resources so the D3D heap descriptors and VRAM are
    // reclaimed while the game is running without the overlay.
    // CPU-side pixel data (icon_decoded_data) is also cleared so it doesn't sit in
    // RAM until the next open; it will be re-decoded from the settings image cache on demand.
    // The RendererResource_t objects themselves (icon / icon_gray) are kept alive because
    // they own no GPU memory when unloaded — the library releases the underlying texture
    // automatically when the last weak_ptr expires.
    if (!state) {
        show_sce_browser = false;
        sce_textures_free_all();
        for (auto &ach : achievements) {
            ach.icon_decoded_data.clear();
            ach.icon_decoded_data.shrink_to_fit();
            ach.icon_gray_decoded_data.clear();
            ach.icon_gray_decoded_data.shrink_to_fit();
            // Detach the pixel buffer from the resource so the GPU texture is released.
            // The resource object stays alive; next GetResourceId() will re-upload.
            if (ach.icon)      ach.icon->ClearAttachedResource();
            if (ach.icon_gray) ach.icon_gray->ClearAttachedResource();
        }
        last_loaded_ach_icon = 0;
    }

    PRINT_DEBUG("%i", (int)state);
    
    Steam_Overlay::allow_renderer_frame_processing(state);
    Steam_Overlay::obscure_game_input(state);

}

void Steam_Overlay::SetLobbyInvite(Friend friendId, uint64 lobbyId)
{
    PRINT_DEBUG("%" PRIu64 " %llu", friendId.id(), lobbyId);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        // Block lobby invites from cross-app friends
        if (settings->get_local_game_id().AppID() != friendId.appid()) return;

        auto& frd = i->second;
        frd.lobbyId = lobbyId;
        frd.window_state |= window_state_lobby_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_rich_invite;
        
        // Add invite to chat history
        std::string invite_msg = "[INVITE] " + i->first.name() + " invited you to join their lobby";
        frd.chat_history.append(invite_msg).append("\n", 1);
        
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::SetRichInvite(Friend friendId, const char* connect_str)
{
    PRINT_DEBUG("%" PRIu64 " '%s'", friendId.id(), connect_str);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        // Block rich invites from cross-app friends
        if (settings->get_local_game_id().AppID() != friendId.appid()) return;

        auto& frd = i->second;
        strncpy(frd.connect, connect_str, k_cchMaxRichPresenceValueLength - 1);
        frd.window_state |= window_state_rich_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_lobby_invite;
        
        // Add invite to chat history
        std::string invite_msg = "[INVITE] " + i->first.name() + " invited you to join their game";
        frd.chat_history.append(invite_msg).append("\n", 1);
        
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::FriendConnect(Friend _friend)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;
    
    int id = find_free_friend_id(friends);
    if (id != 0) {
        auto& item = friends[_friend];
        item.window_title = std::move(_friend.name() + " " + translationPlaying[current_language] + " " + std::to_string(_friend.appid()));
        item.window_state = window_state_none;
        item.id = id;
        memset(item.chat_input, 0, max_chat_len);
        item.joinable = false;
    } else {
        PRINT_DEBUG("error no free id to create a friend window");
    }
}

void Steam_Overlay::FriendDisconnect(Friend _friend)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;
    
    auto it = friends.find(_friend);
    if (it != friends.end())
        friends.erase(it);
}

void Steam_Overlay::FriendUpdate(Friend _friend)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("%" PRIu64 " lobby_id=%" PRIu64 "", _friend.id(), _friend.lobby_id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    // Find existing friend entry by ID
    std::pair<const Friend, friend_window_state>* updated_frd = nullptr;
    for (auto it = friends.begin(); it != friends.end(); ++it) {
        if (it->first.id() == _friend.id()) {
            // Update the Friend key in-place (Friend_Less only compares by id(),
            // so map ordering is preserved — no need to erase and reinsert)
            Friend &frd_ref = const_cast<Friend&>(it->first);
            frd_ref = _friend;
            // Update window title with new appid
            it->second.window_title = _friend.name() + " " + translationPlaying[current_language] + " " + std::to_string(_friend.appid());
            updated_frd = &(*it);
            break;
        }
    }

    if (!updated_frd) return;

    // Check if a same-app friend entered a lobby we haven't notified about yet
    uint64 friend_id = _friend.id();
    uint64 friend_lobby = _friend.lobby_id();
    bool same_app = (settings->get_local_game_id().AppID() == _friend.appid());

    // Skip if we're already in that lobby
    uint64 my_lobby = settings->get_lobby().ConvertToUint64();

    if (same_app && friend_lobby != 0 && friend_lobby != my_lobby) {
        auto tracked = notified_friend_lobbies.find(friend_id);
        if (tracked == notified_friend_lobbies.end() || tracked->second != friend_lobby) {
            // New lobby or different lobby — show notification
            notified_friend_lobbies[friend_id] = friend_lobby;
            std::string msg = _friend.name() + " is in a lobby";
            Notification notif{};
            notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
            notif.id = find_free_notification_id(notifications);
            if (notif.id != 0) {
                notif.type = (uint8)notification_type::friend_lobby_available;
                notif.message = msg;
                notif.frd = updated_frd;
                notif.source_friend_id = friend_id;
                notif.join_request_lobby_id = friend_lobby;
                notifications.emplace_back(notif);
                allow_renderer_frame_processing(true);
                obscure_game_input(true);
                PRINT_DEBUG("friend %" PRIu64 " lobby notification for lobby %" PRIu64 "", friend_id, friend_lobby);
            }
        }
    } else {
        // Friend left lobby or different app — clear tracking
        notified_friend_lobbies.erase(friend_id);
    }
}

// show a notification when the user unlocks an achievement
void Steam_Overlay::AddAchievementNotification(const std::string &ach_name, nlohmann::json const &ach, bool for_progress)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("'%s' %i", ach_name.c_str(), (int)for_progress);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    // don't return early when disable_overlay_achievement_notification is true
    // otherwise when you open the achievements list/menu you won't see the new unlock status

    for (auto &a : achievements) {
        if (a.name == ach_name) {
            try {
                // lock to prevent modifications to this json object
                std::lock_guard<std::recursive_mutex> lock2(global_mutex);

                a.achieved = ach.value("earned", false);
                a.unlock_time = ach.value("earned_time", static_cast<uint32>(0));
                a.progress = ach.value("progress", static_cast<uint32>(0));
                a.max_progress = ach.value("max_progress", static_cast<uint32>(0));
            } catch(...) {}

            if (a.achieved && !for_progress) { // here we don't show the progress indications
                post_achievement_notification(a, for_progress);
                notify_sound_user_achievement();
            } else if (for_progress && !settings->disable_overlay_achievement_progress) { // progress indication is shown for locked achievements only
                // post notification if this isn't a progress, or a progress and the user didn't disable these notifications
                post_achievement_notification(a, for_progress);
                // don't play sound
            }
            break;
        }
    }
}



// -- steam run callbacks --
void Steam_Overlay::steam_run_callback_update_my_lobby()
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;

    // Detect lobby state transitions
    bool had_lobby = i_have_lobby;
    if (std::string(steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect")).length() > 0) {
        i_have_lobby = true;
    } else if (settings->get_lobby().IsValid()) {
        i_have_lobby = true;
    } else {
        i_have_lobby = false;
    }
    if (!had_lobby && i_have_lobby) {
        CSteamID lobby = settings->get_lobby();
        bool is_owner = lobby.IsValid() && get_steam_client()->steam_matchmaking &&
            get_steam_client()->steam_matchmaking->GetLobbyOwner(lobby) == settings->get_local_steam_id();
        if (is_owner) {
            std::string msg = "Lobby created (" + std::to_string(lobby.ConvertToUint64()) + ")";
            if (!submit_notification(notification_type::lobby_status, msg)) {
                pending_lobby_notifications.push_back(msg);
            }
        }
    } else if (had_lobby && !i_have_lobby) {
        if (!submit_notification(notification_type::lobby_status, "Lobby closed")) {
            pending_lobby_notifications.push_back("Lobby closed");
        }
        // Clear lobby chat history
        lobby_chat_history.clear();
        lobby_chat_last_entry_count = 0;
        lobby_chat_input[0] = '\0';
    }

    // Detect game server state transitions
    bool had_server = i_have_game_server;
    Steam_GameServer *gs = get_steam_client()->steam_gameserver;
    bool have_server = gs && gs->BLoggedOn();
    i_have_game_server = have_server;
    if (!had_server && have_server) {
        const auto &sd = gs->get_server_data();
        std::string sname = sd.server_name();
        std::string msg = "Game server started";
        if (!sname.empty()) msg += " (" + sname + ")";
        if (!submit_notification(notification_type::lobby_status, msg)) {
            pending_lobby_notifications.push_back(msg);
        }
    } else if (had_server && !have_server) {
        if (!submit_notification(notification_type::lobby_status, "Game server stopped")) {
            pending_lobby_notifications.push_back("Game server stopped");
        }
    }
}

bool Steam_Overlay::is_friend_joinable(std::pair<const Friend, friend_window_state> &f)
{
    PRINT_DEBUG("%" PRIu64 "", f.first.id());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;

    if (std::string(steamFriends->get_friend_rich_presence_silent((uint64)f.first.id(), "connect")).length() > 0 ) {
        PRINT_DEBUG("%" PRIu64 " true (connect string)", f.first.id());
        return true;
    }

    FriendGameInfo_t friend_game_info{};
    steamFriends->GetFriendGamePlayed((uint64)f.first.id(), &friend_game_info);
    if (friend_game_info.m_steamIDLobby.IsValid()) {
        PRINT_DEBUG("%" PRIu64 " true (friend in a lobby)", f.first.id());
        return true;
    }

    PRINT_DEBUG("%" PRIu64 " false", f.first.id());
    return false;
}

void Steam_Overlay::invite_friend(uint64 friend_id, class Steam_Friends* steamFriends, class Steam_Matchmaking* steamMatchmaking)
{
    std::string connect_str = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
    if (connect_str.length() > 0) {
        steamFriends->InviteUserToGame(friend_id, connect_str.c_str());
        PRINT_DEBUG("sent game invitation to friend with id = %llu", friend_id);
    } else if (settings->get_lobby().IsValid()) {
        steamMatchmaking->InviteUserToLobby(settings->get_lobby(), friend_id);
        PRINT_DEBUG("sent lobby invitation to friend with id = %llu", friend_id);
    }
}

void Steam_Overlay::steam_run_callback_friends_actions()
{
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    CSteamID my_lobby_action = settings->get_lobby();
    std::for_each(friends.begin(), friends.end(), [this, &my_lobby_action](std::pair<Friend const, friend_window_state> &i) {
        i.second.joinable = is_friend_joinable(i);

        // Auto-clear stale/invalid invites
        if (i.second.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            bool clear_invite = false;

            // Already in the friend's lobby — no need for the invite
            if (my_lobby_action.IsValid() && my_lobby_action.ConvertToUint64() == i.first.lobby_id()) {
                clear_invite = true;
            }

            // Lobby invite but friend no longer has a lobby
            if ((i.second.window_state & window_state_lobby_invite) && i.first.lobby_id() == 0) {
                clear_invite = true;
            }

            // Rich invite but friend no longer has a connect string
            if (i.second.window_state & window_state_rich_invite) {
                Steam_Friends* sf = get_steam_client()->steam_friends;
                if (sf && std::string(sf->get_friend_rich_presence_silent((uint64)i.first.id(), "connect")).empty()) {
                    clear_invite = true;
                }
            }

            // Friend is no longer joinable at all
            if (!i.second.joinable) {
                clear_invite = true;
            }

            if (clear_invite) {
                i.second.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
            }
        }
    });

    while (!has_friend_action.empty()) {
        auto friend_info = friends.find(has_friend_action.front());
        if (friend_info != friends.end()) {
            uint64 friend_id = (uint64)friend_info->first.id();
            // The user clicked on "Send"
            if (friend_info->second.window_state & window_state_send_message) {
                char* input = friend_info->second.chat_input;
                char* end_input = input + strlen(input);
                char* printable_char = std::find_if(input, end_input, [](char c) { return std::isgraph(c); });

                // Check if the message contains something else than blanks
                if (printable_char != end_input) {
                    // Handle chat send
                    Common_Message msg;
                    Steam_Messages* steam_messages = new Steam_Messages;
                    steam_messages->set_type(Steam_Messages::FRIEND_CHAT);
                    steam_messages->set_message(friend_info->second.chat_input);
                    msg.set_allocated_steam_messages(steam_messages);
                    msg.set_source_id(settings->get_local_steam_id().ConvertToUint64());
                    msg.set_dest_id(friend_id);
                    network->sendTo(&msg, true, NULL, settings->enable_crossapp_messaging);

                    friend_info->second.chat_history.append(get_steam_client()->settings_client->get_local_name()).append(": ").append(input).append("\n", 1);
                }
                *input = 0; // Reset the input field

                friend_info->second.window_state &= ~window_state_send_message;
            }
            // The user clicked on "Invite" (but invite all wasn't clicked)
            if (friend_info->second.window_state & window_state_invite) {
                // Only allow invite for same-app friends
                if (settings->get_local_game_id().AppID() == friend_info->first.appid()) {
                    invite_friend(friend_id, steamFriends, steamMatchmaking);
                }
                friend_info->second.window_state &= ~window_state_invite;
            }
            // The user clicked on "Join"
            if (friend_info->second.window_state & window_state_join) {
                // Only allow join for same-app friends
                if (settings->get_local_game_id().AppID() != friend_info->first.appid()) {
                    friend_info->second.window_state &= ~(window_state_join | window_state_lobby_invite | window_state_rich_invite);
                } else {
                    std::string connect = steamFriends->get_friend_rich_presence_silent(friend_id, "connect");
                    // The user got a lobby invite and accepted it
                    if (friend_info->second.window_state & window_state_lobby_invite) {
                        GameLobbyJoinRequested_t data;
                        data.m_steamIDLobby.SetFromUint64(friend_info->second.lobbyId);
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));

                        friend_info->second.window_state &= ~window_state_lobby_invite;
                    } else {
                        // The user got a rich presence invite and accepted it
                        if (friend_info->second.window_state & window_state_rich_invite) {
                            GameRichPresenceJoinRequested_t data = {};
                            data.m_steamIDFriend.SetFromUint64(friend_id);
                            strncpy(data.m_rgchConnect, friend_info->second.connect, k_cchMaxRichPresenceValueLength - 1);
                            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                            
                            friend_info->second.window_state &= ~window_state_rich_invite;
                        } else if (connect.length() > 0) {
                            GameRichPresenceJoinRequested_t data = {};
                            data.m_steamIDFriend.SetFromUint64(friend_id);
                            strncpy(data.m_rgchConnect, connect.c_str(), k_cchMaxRichPresenceValueLength - 1);
                            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                        }

                        //Not sure about this but it fixes sonic racing transformed invites
                        FriendGameInfo_t friend_game_info = {};
                        steamFriends->GetFriendGamePlayed(friend_id, &friend_game_info);
                        uint64 lobby_id = friend_game_info.m_steamIDLobby.ConvertToUint64();
                        if (lobby_id) {
                            GameLobbyJoinRequested_t data;
                            data.m_steamIDLobby.SetFromUint64(lobby_id);
                            data.m_steamIDFriend.SetFromUint64(friend_id);
                            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                        }
                    }
                }
                
                friend_info->second.window_state &= ~window_state_join;
            }
        }
        has_friend_action.pop();
    }

}

void Steam_Overlay::steam_run_callback()
{
    // Track lobby/server state even before overlay is ready so we don't miss transitions
    steam_run_callback_update_my_lobby();

    if (!Ready()) return;

    // Flush any notifications that were queued before the overlay was ready
    if (!pending_lobby_notifications.empty()) {
        for (auto &msg : pending_lobby_notifications) {
            submit_notification(notification_type::lobby_status, msg);
        }
        pending_lobby_notifications.clear();
    }

    if (overlay_state_changed) {
        overlay_state_changed = false;

        if (!settings->disable_overlay_activated_callback) {
            GameOverlayActivated_t data{};
            data.m_bActive = show_overlay;
            data.m_bUserInitiated = true;
            data.m_dwOverlayPID = 123;
            data.m_nAppID = settings->get_local_game_id().AppID();
            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
        }
    }

    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    if (save_settings) {
        save_settings = false;

        const char *language_text = valid_languages[current_language];
        save_global_settings(get_steam_client()->local_storage, username_text, language_text);
        get_steam_client()->settings_client->set_local_name(username_text);
        get_steam_client()->settings_server->set_local_name(username_text);
        get_steam_client()->settings_client->set_language(language_text);
        get_steam_client()->settings_server->set_language(language_text);
        steamFriends->resend_friend_data();
    }

    // if variable == true, then set it to false and return true (because state was changed) in that case
    bool yes_clicked = true;
    if (invite_all_friends_clicked.compare_exchange_weak(yes_clicked, false)) {
        PRINT_DEBUG("Steam_Overlay will send invitations to [%zu] friends if they're using the same app", friends.size());
        uint32 current_appid = settings->get_local_game_id().AppID();
        for (auto &fr : friends) {
            if (fr.first.appid() == current_appid) { // friend is playing the same game
                uint64 friend_id = (uint64)fr.first.id();
                invite_friend(friend_id, steamFriends, steamMatchmaking);
            }
        }
    }

    // don't wait to lock the overlay mutex
    // * the overlay proc might be active and holding the overlay mutex
    // * this steam callback will be blocked, but it has the global mutex locked
    // * the overlay proc tries to lock the global mutex, but since we have it, it will be blocked forever
    if (overlay_mutex.try_lock()) {
        if (Ready()) {
            // ==============================================================
            // call steam callbacks that has to change the overlay state here
            // ==============================================================

            steam_run_callback_friends_actions();
            poll_lobby_join_requests();
        }

        overlay_mutex.unlock();
    }
}



// -- steam networking callbacks --
void Steam_Overlay::networking_msg_received(Common_Message *msg)
{
    if (msg->has_steam_messages()) {
        std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
        Friend frd;
        frd.set_id(msg->source_id());
        auto friend_info = friends.find(frd);
        if (friend_info != friends.end()) {
            Steam_Messages const& steam_message = msg->steam_messages();
            // Change color to cyan for friend
            friend_info->second.chat_history.append(friend_info->first.name() + ": " + steam_message.message()).append("\n", 1);

            // Auto-open the friend's chat tab if the chat window is already visible
            if (show_chat) {
                friend_info->second.window_state |= window_state_show;
            }

            if (!(friend_info->second.window_state & window_state_show)) {
                friend_info->second.window_state |= window_state_need_attention;
            }

            add_chat_message_notification(friend_info->first.name() + ": " + steam_message.message(), &(*friend_info));
            notify_sound_user_invite(friend_info->second);
        }
    }
}


/* ══════════════════════════════════════════════════════════════════════════
 *  Bridge accessor methods — called by overlay_bridge.cpp (C ABI exports).
 *  These run on whatever thread the addon calls from (ReShade render thread).
 *  We lock overlay_mutex for thread safety.
 * ══════════════════════════════════════════════════════════════════════════ */

bool Steam_Overlay::Bridge_GetWarnLocalSave() const
{
    return warn_local_save;
}

bool Steam_Overlay::Bridge_GetWarnBadAppId() const
{
    return warn_bad_appid;
}

int Steam_Overlay::Bridge_GetNotifPosition() const
{
    // Map ENotificationPosition to GSE_NotifPosition
    switch (notif_position) {
    case k_EPositionTopLeft:     return GSE_NOTIF_POS_TOP_LEFT;
    case k_EPositionTopRight:    return GSE_NOTIF_POS_TOP_RIGHT;
    case k_EPositionBottomLeft:  return GSE_NOTIF_POS_BOT_LEFT;
    case k_EPositionBottomRight: return GSE_NOTIF_POS_BOT_RIGHT;
    default:                     return GSE_NOTIF_POS_BOT_LEFT;
    }
}

Steam_Overlay::BridgeStatsSnapshot Steam_Overlay::Bridge_GetStatsState() const
{
    BridgeStatsSnapshot s{};
    s.show_fps       = stats.show_fps;
    s.show_frametime = stats.show_frametime;
    s.show_playtime  = stats.show_playtime;
    s.show_fps_graph       = stats.show_fps_graph;
    s.show_frametime_graph = stats.show_frametime_graph;
    s.show_min_max_avg     = stats.show_min_max_avg;
    s.show_percentile_1    = stats.show_percentile_1;
    s.show_percentile_5    = stats.show_percentile_5;
    s.show_percentile_01   = stats.show_percentile_01;
    s.graph_timeframe_sec  = stats.graph_timeframe_sec;
    // Stats values are updated on the render thread; we read them directly.
    // The addon will compute its own FPS/frametime since it has its own frame loop.
    s.fps = 0;
    s.frametime_ms = 0;
    s.playtime_hr = 0;
    s.playtime_min = 0;
    s.playtime_sec = 0;
    return s;
}

int Steam_Overlay::Bridge_GetAchievementCount() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));
    return static_cast<int>(achievements.size());
}

static void bridge_safe_copy(char *dst, size_t dst_sz, const std::string &src)
{
    if (!dst || !dst_sz) return;
    strncpy(dst, src.c_str(), dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

int Steam_Overlay::Bridge_GetAchievements(GSE_Achievement *out, int max_count)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // Build a map from achievement API name to group index
    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    std::unordered_map<std::string, int16_t> name_to_group;
    if (steamUserStats && steamUserStats->steamhunters_data_populated) {
        int16_t group_idx = 0;
        for (const auto &grp : steamUserStats->steamhunters_achievement_groups) {
            for (const auto &api_name : grp.achievementApiNames) {
                name_to_group[api_name] = group_idx;
            }
            ++group_idx;
        }
    }

    int count = (std::min)(max_count, static_cast<int>(achievements.size()));
    for (int i = 0; i < count; ++i) {
        auto &a = achievements[i];
        auto &o = out[i];
        memset(&o, 0, sizeof(o));

        bridge_safe_copy(o.name, sizeof(o.name), a.name);
        bridge_safe_copy(o.title, sizeof(o.title), a.title);
        bridge_safe_copy(o.description, sizeof(o.description), a.description);
        o.progress = a.progress;
        o.max_progress = a.max_progress;
        o.unlock_time = a.unlock_time;
        o.hidden = a.hidden ? 1 : 0;
        o.achieved = a.achieved ? 1 : 0;

        // Group index (-1 = ungrouped/base game)
        auto git = name_to_group.find(a.name);
        o.group_index = (git != name_to_group.end()) ? git->second : -1;

        // Global percentage
        auto it = ach_global_percentages.find(a.name);
        o.global_percent = (it != ach_global_percentages.end()) ? it->second : -1.0f;

        // SteamHunters per-achievement data
        o.sh_local_percent = -1.0f;
        o.sh_points = 0;
        o.obtainability = 0;
        o._pad = 0;
        if (steamUserStats && steamUserStats->steamhunters_data_populated) {
            auto sit = steamUserStats->steamhunters_achievement_data.find(a.name);
            if (sit != steamUserStats->steamhunters_achievement_data.end()) {
                o.sh_local_percent = sit->second.localPercentage;
                o.sh_points = static_cast<int16_t>(sit->second.points);
                o.obtainability = static_cast<uint8_t>(sit->second.obtainability);
            }
        }

        // Decode icon pixel data on demand for the bridge.
        // When the ReShade addon is connected, overlay_render_proc() returns early
        // and never calls try_load_ach_icon(), so icon_decoded_data stays empty.
        // We decode from the settings image cache here (CPU-only, no GPU upload).
        auto bridge_ensure_icon = [&](bool achieved) {
            auto &decoded = achieved ? a.icon_decoded_data : a.icon_gray_decoded_data;
            if (!decoded.empty()) return; // already decoded

            int &handle = achieved ? a.icon_handle : a.icon_gray_handle;
            if (Settings::UNLOADED_IMAGE_HANDLE == handle) {
                handle = get_steam_client()->steam_user_stats->get_achievement_icon_handle(a.name, achieved);
            }
            auto *img = settings->get_image(handle);
            if (!img) return;
            int iw = static_cast<int>(img->width);
            int ih = static_cast<int>(img->height);
            if (iw <= 0 || ih <= 0 || img->data.size() < (size_t)iw * ih * 4) return;
            decoded = img->data;
        };
        bridge_ensure_icon(true);
        bridge_ensure_icon(false);

        // Icon pixel data — point directly into decoded data buffers.
        // These are valid until the next call (the emu won't modify them while
        // the bridge is being called, because the render thread is us).
        if (!a.icon_decoded_data.empty()) {
            o.icon_pixels = reinterpret_cast<const uint8_t*>(a.icon_decoded_data.data());
            o.icon_w = 64;  // achievement icons are always 64x64
            o.icon_h = 64;
        }
        if (!a.icon_gray_decoded_data.empty()) {
            o.icon_gray_pixels = reinterpret_cast<const uint8_t*>(a.icon_gray_decoded_data.data());
            o.icon_gray_w = 64;
            o.icon_gray_h = 64;
        }
    }
    return count;
}

int Steam_Overlay::Bridge_HasAchievementGroups()
{
    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats) return 0;
    return (steamUserStats->steamhunters_data_populated
            && !steamUserStats->steamhunters_achievement_groups.empty()) ? 1 : 0;
}

int Steam_Overlay::Bridge_GetAchievementGroupCount()
{
    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats || !steamUserStats->steamhunters_data_populated) return 0;
    return static_cast<int>(steamUserStats->steamhunters_achievement_groups.size());
}

int Steam_Overlay::Bridge_GetAchievementGroups(GSE_AchievementGroup *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats || !steamUserStats->steamhunters_data_populated) return 0;

    const auto &groups = steamUserStats->steamhunters_achievement_groups;
    int count = (std::min)(max_count, static_cast<int>(groups.size()));
    for (int i = 0; i < count; ++i) {
        const auto &g = groups[i];
        auto &o = out[i];
        memset(&o, 0, sizeof(o));

        bridge_safe_copy(o.name, sizeof(o.name), g.name);
        bridge_safe_copy(o.dlc_app_name, sizeof(o.dlc_app_name), g.dlcAppName);
        o.dlc_app_id = g.dlcAppId;
        o.achievement_count = static_cast<int32_t>(g.achievementApiNames.size());
    }
    return count;
}

int Steam_Overlay::Bridge_GetNotifications(GSE_Notification *out, int max_count)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    int written = 0;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    for (auto &n : notifications) {
        if (n.expired) continue;
        if (written >= max_count) break;

        auto &o = out[written];
        memset(&o, 0, sizeof(o));

        o.id = n.id;
        o.type = n.type;
        o.start_time_ms = n.steady_start_time.count();

        auto duration = get_notification_duration(static_cast<notification_type>(n.type));
        o.duration_ms = duration.count();

        bridge_safe_copy(o.message, sizeof(o.message), n.message);

        // Achievement data if present
        if (n.ach.has_value()) {
            auto &a = n.ach.value();
            bridge_safe_copy(o.ach_title, sizeof(o.ach_title), a.title);
            o.ach_progress = a.progress;
            o.ach_max_progress = a.max_progress;
            o.ach_achieved = a.achieved ? 1 : 0;

            // On-demand icon decode for the bridge (same pattern as Bridge_GetAchievements)
            if (a.icon_decoded_data.empty() && a.icon_handle != Settings::UNLOADED_IMAGE_HANDLE) {
                auto *img = settings->get_image(a.icon_handle);
                if (img && img->width > 0 && img->height > 0 &&
                    img->data.size() >= (size_t)img->width * img->height * 4)
                    a.icon_decoded_data = img->data;
            }
            if (a.icon_decoded_data.empty() && !a.name.empty()) {
                // Try to find the source achievement and decode from its handle
                for (auto &src : achievements) {
                    if (src.name == a.name) {
                        int &h = a.achieved ? src.icon_handle : src.icon_gray_handle;
                        if (Settings::UNLOADED_IMAGE_HANDLE == h)
                            h = get_steam_client()->steam_user_stats->get_achievement_icon_handle(src.name, a.achieved);
                        auto *img = settings->get_image(h);
                        if (img && img->width > 0 && img->height > 0 &&
                            img->data.size() >= (size_t)img->width * img->height * 4)
                            a.icon_decoded_data = img->data;
                        break;
                    }
                }
            }

            if (!a.icon_decoded_data.empty()) {
                o.ach_icon_pixels = reinterpret_cast<const uint8_t*>(a.icon_decoded_data.data());
                o.ach_icon_w = 64;
                o.ach_icon_h = 64;
            }
        }

        // Lobby join request data
        o.join_request_lobby_id = n.join_request_lobby_id;
        o.join_request_requester_id = n.join_request_requester_id;

        // Source friend for invite/message notifications
        if (n.frd) {
            o.source_friend_id = n.frd->first.id();
        } else if (n.source_friend_id) {
            o.source_friend_id = n.source_friend_id;
        }

        ++written;
    }

    // In bridge mode, build_notifications() never runs, so expired notifications
    // would pile up forever. Clean them up here.
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [this](const Notification &item) {
                if (!item.expired) return false;
                // Clean up lobby join request tracking (same as native expiry)
                if ((notification_type)item.type == notification_type::lobby_join_request) {
                    notified_lobby_join_requests.erase({item.join_request_lobby_id, item.join_request_requester_id});
                }
                return true;
            }),
        notifications.end());

    return written;
}

void Steam_Overlay::Bridge_ExpireNotification(int id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == id && !n.expired) {
            // Clean up lobby join request tracking on expiry
            if ((notification_type)n.type == notification_type::lobby_join_request) {
                notified_lobby_join_requests.erase({n.join_request_lobby_id, n.join_request_requester_id});
            }
            n.expired = true;
            allow_renderer_frame_processing(false);
            break;
        }
    }
}

int Steam_Overlay::Bridge_GetDisplayInfo(GSE_DisplayInfo *out, int max_count) const
{
    // Call the static display query function (thread-safe, reads from Windows API)
    auto displays = query_display_hdr_details();
    int count = (std::min)(max_count, static_cast<int>(displays.size()));

    for (int i = 0; i < count; ++i) {
        const auto &d = displays[i];
        auto &o = out[i];
        memset(&o, 0, sizeof(o));

        bridge_safe_copy(o.name, sizeof(o.name), d.name);
        bridge_safe_copy(o.encoding, sizeof(o.encoding), d.encoding);
        bridge_safe_copy(o.gamut, sizeof(o.gamut), d.gamut);
        bridge_safe_copy(o.transfer, sizeof(o.transfer), d.transfer);
        bridge_safe_copy(o.range, sizeof(o.range), d.range);
        o.hdr_supported = d.hdr_supported ? 1 : 0;
        o.hdr_enabled = d.hdr_enabled ? 1 : 0;
        o.wide_color = d.wide_color ? 1 : 0;
        o.force_disabled = d.force_disabled ? 1 : 0;
        o.bpc = d.bpc;
        o.sdr_white_nits = d.sdr_white_nits;
    }
    return count;
}

float Steam_Overlay::Bridge_GetSDRWhiteScale() const
{
    return s_sdr_white_scale;
}

// Helper: format a host-byte-order uint32 IP as dotted-quad (e.g. "192.168.1.1")
static void format_ip_address(uint32 ip, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    if (ip == 0) { buf[0] = '\0'; return; }
    snprintf(buf, len, "%u.%u.%u.%u",
        (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

int Steam_Overlay::Bridge_GetFriendCount() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));
    return static_cast<int>(friends.size());
}

int Steam_Overlay::Bridge_GetFriends(GSE_Friend *out, int max_count) const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));

    uint32 local_appid = settings->get_local_game_id().AppID();

    int written = 0;
    for (const auto &[frd, state] : friends) {
        if (written >= max_count) break;

        auto &o = out[written];
        memset(&o, 0, sizeof(o));

        o.steam_id = frd.id();
        bridge_safe_copy(o.name, sizeof(o.name), frd.name());
        o.is_online = 1; // Friends in this map are online (FriendConnect adds, FriendDisconnect removes)
        o.is_joinable = state.joinable ? 1 : 0;
        o.same_app = (frd.appid() == local_appid) ? 1 : 0;
        o.window_state = state.window_state;
        o.has_pending_invite = (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) ? 1 : 0;
        o.appid = frd.appid();
        o.lobby_id = frd.lobby_id();
        o.in_lobby = (frd.lobby_id() != 0) ? 1 : 0;

        // Check if this friend is actually a member of the local user's lobby
        // (using the lobby member list, which is more reliable than frd.lobby_id())
        CSteamID my_lobby = settings->get_lobby();
        o.in_my_lobby = 0;
        if (my_lobby.IsValid()) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            if (mm) {
                int member_count = mm->GetNumLobbyMembers(my_lobby);
                for (int mi = 0; mi < member_count; ++mi) {
                    if (mm->GetLobbyMemberByIndex(my_lobby, mi).ConvertToUint64() == frd.id()) {
                        o.in_my_lobby = 1;
                        break;
                    }
                }
            }
        }

        // Resolve appid → game name
        if (o.appid != 0) {
            auto it = steam_preowned_app_ids.find(o.appid);
            if (it != steam_preowned_app_ids.end()) {
                bridge_safe_copy(o.app_name, sizeof(o.app_name), it->second);
            }
        }

        // Get friend's connect string
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        if (steamFriends) {
            std::string connect = steamFriends->get_friend_rich_presence_silent(CSteamID((uint64)frd.id()), "connect");
            if (!connect.empty()) {
                strncpy(o.connect_string, connect.c_str(), GSE_CONNECT_STRING_SIZE - 1);
                o.connect_string[GSE_CONNECT_STRING_SIZE - 1] = '\0';
            }
        }

        // Get friend's lobby details (owner name, member count/limit)
        if (o.in_lobby) {
            // Try local matchmaking data first (available for same-app lobbies)
            Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
            if (matchmaking) {
                CSteamID friend_lobby((uint64)frd.lobby_id());
                CSteamID owner = matchmaking->GetLobbyOwner(friend_lobby);
                if (owner.IsValid() && steamFriends) {
                    const char *oname = steamFriends->GetFriendPersonaName(owner);
                    if (oname && oname[0]) {
                        bridge_safe_copy(o.lobby_owner_name, sizeof(o.lobby_owner_name), std::string(oname));
                    }
                }
                o.lobby_member_count = matchmaking->GetNumLobbyMembers(friend_lobby);
                o.lobby_member_limit = matchmaking->GetLobbyMemberLimit(friend_lobby);
            }

            // Fall back to proto fields for cross-app lobbies
            if (o.lobby_member_count == 0 && frd.lobby_member_count() > 0)
                o.lobby_member_count = frd.lobby_member_count();
            if (o.lobby_member_limit == 0 && frd.lobby_member_limit() > 0)
                o.lobby_member_limit = frd.lobby_member_limit();
            if (o.lobby_owner_name[0] == '\0' && frd.lobby_owner_name().size() > 0)
                bridge_safe_copy(o.lobby_owner_name, sizeof(o.lobby_owner_name), frd.lobby_owner_name());
        }

        // Get friend's detected IP addresses from the networking layer
        if (network) {
            uint32 frd_ips[16];
            int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
            o.ip_str[0] = '\0';
            size_t pos = 0;
            for (int k = 0; k < frd_ip_count && pos < sizeof(o.ip_str) - 20; ++k) {
                if (frd_ips[k] == 0) continue;
                if (pos > 0) { o.ip_str[pos++] = ','; o.ip_str[pos++] = ' '; }
                char tmp[24];
                format_ip_address(frd_ips[k], tmp, sizeof(tmp));
                size_t len = strlen(tmp);
                memcpy(o.ip_str + pos, tmp, len);
                pos += len;
            }
            o.ip_str[pos] = '\0';
        }

        ++written;
    }
    return written;
}

int Steam_Overlay::Bridge_HasLobby() const
{
    // Same logic as i_have_lobby in steam_run_callback_update_my_lobby()
    return i_have_lobby ? 1 : 0;
}

int Steam_Overlay::Bridge_GetLocalLobbyInfo(GSE_LocalLobbyInfo *out) const
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    if (!i_have_lobby) return 0;

    CSteamID lobby = settings->get_lobby();
    if (!lobby.IsValid()) return 0;

    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
    if (!matchmaking) return 0;

    out->lobby_id = lobby.ConvertToUint64();
    out->lobby_owner = matchmaking->GetLobbyOwner(lobby).ConvertToUint64();
    out->member_count = matchmaking->GetNumLobbyMembers(lobby);
    out->member_limit = matchmaking->GetLobbyMemberLimit(lobby);
    out->is_owner = (out->lobby_owner == settings->get_local_steam_id().ConvertToUint64()) ? 1 : 0;

    // Also include connect string if set
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (steamFriends) {
        std::string connect = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
        if (!connect.empty()) {
            strncpy(out->connect_string, connect.c_str(), GSE_CONNECT_STRING_SIZE - 1);
            out->connect_string[GSE_CONNECT_STRING_SIZE - 1] = '\0';
        }
    }

    // Include game exe filename
    {
        std::string full_path = get_full_exe_path();
        std::string::size_type pos = full_path.find_last_of("/\\");
        std::string name = (pos != std::string::npos) ? full_path.substr(pos + 1) : full_path;
        strncpy(out->exe_name, name.c_str(), GSE_EXE_NAME_SIZE - 1);
        out->exe_name[GSE_EXE_NAME_SIZE - 1] = '\0';
    }

    return 1;
}

int Steam_Overlay::Bridge_GetConnectString(char *out, int out_size) const
{
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (!steamFriends) return 0;

    std::string connect = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
    if (connect.empty()) return 0;

    strncpy(out, connect.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

int Steam_Overlay::Bridge_GetExeName(char *out, int out_size) const
{
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    std::string full_path = get_full_exe_path();
    std::string::size_type pos = full_path.find_last_of("/\\");
    std::string name = (pos != std::string::npos) ? full_path.substr(pos + 1) : full_path;
    if (name.empty()) return 0;

    strncpy(out, name.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

int Steam_Overlay::Bridge_GetGameServerInfo(GSE_GameServerInfo *out) const
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    Steam_GameServer *gs = get_steam_client()->steam_gameserver;
    if (!gs || !gs->BLoggedOn()) return 0;

    const auto &sd = gs->get_server_data();
    out->active = 1;
    out->num_players = sd.num_players();
    out->max_players = sd.max_player_count();
    out->bot_players = sd.bot_player_count();
    bridge_safe_copy(out->server_name, sizeof(out->server_name), sd.server_name());
    bridge_safe_copy(out->map_name, sizeof(out->map_name), sd.map_name());
    return 1;
}

int Steam_Overlay::Bridge_GetLanguage() const
{
    return current_language;
}

void Steam_Overlay::Bridge_RequestSaveSettings()
{
    save_settings = true;
}

// If the addon hasn't called GetState() in this long, consider it disconnected
// and let the native overlay resume rendering (when enabled).
static constexpr int64_t BRIDGE_HEARTBEAT_TIMEOUT_MS = 5000;

static int64_t bridge_now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Steam_Overlay::Bridge_MarkConnected()
{
    auto prev = bridge_last_heartbeat_ms.exchange(bridge_now_ms(), std::memory_order_relaxed);
    if (prev == 0) {
        // First connection — set is_ready and late_init_imgui so Ready() returns true.
        // Needed for bridge-only mode where native hooks don't set these.
        std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
        late_init_imgui.store(true, std::memory_order_relaxed);
        is_ready = true;

        // In bridge-only mode renderer_hook_proc never runs, so achievements
        // and audio were never loaded. Do it now (skips GPU resource creation
        // since _renderer is null).
        if (!_renderer && achievements.empty()) {
            load_achievements_data();
            load_audio();
        }
    }
}

bool Steam_Overlay::Bridge_IsConnected() const
{
    auto last = bridge_last_heartbeat_ms.load(std::memory_order_relaxed);
    if (last == 0) return false;
    return (bridge_now_ms() - last) < BRIDGE_HEARTBEAT_TIMEOUT_MS;
}

void Steam_Overlay::Bridge_TestAchievement()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    show_test_achievement();
}

void Steam_Overlay::Bridge_ResetAchievements()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    if (achievements_snapshot.empty()) return;
    for (auto &ax : achievements) {
        for (const auto &snap : achievements_snapshot) {
            if (snap.name == ax.name) {
                ax.achieved    = snap.achieved;
                ax.progress    = snap.progress;
                ax.unlock_time = snap.unlock_time;
                break;
            }
        }
    }
    ach_global_percentages = ach_global_percentages_snapshot;
}

void Steam_Overlay::Bridge_SimulateAchievements()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> percent_dist(0, 100);
    std::uniform_real_distribution<float> global_dist(0.5f, 99.9f);
    std::uniform_int_distribution<uint32_t> time_dist(0, 5u * 24u * 3600u);
    const uint32_t now_ts = (uint32_t)std::time(nullptr);

    for (auto &ax : achievements) {
        if (ax.hidden) continue;
        uint32_t roll = percent_dist(rng);
        if (ax.max_progress > 0) {
            if (roll < 40) {
                ax.achieved = true;
                ax.progress = ax.max_progress;
                ax.unlock_time = now_ts - time_dist(rng);
            } else if (roll < 80) {
                ax.achieved = false;
                std::uniform_int_distribution<uint32_t> prog_dist(1, ax.max_progress > 1 ? ax.max_progress - 1 : 1);
                ax.progress = prog_dist(rng);
                ax.unlock_time = 0;
            } else {
                ax.achieved = false;
                ax.progress = 0;
                ax.unlock_time = 0;
            }
        } else {
            // regular achievement: ~40% achieved, ~30% in-progress (fake), ~30% locked
            if (roll < 40) {
                ax.achieved = true;
                ax.unlock_time = now_ts - time_dist(rng);
                ax.max_progress = 0;
                ax.progress = 0;
            } else if (roll < 70) {
                ax.achieved = false;
                ax.unlock_time = 0;
                std::uniform_int_distribution<uint32_t> fake_max(5, 50);
                ax.max_progress = fake_max(rng);
                std::uniform_int_distribution<uint32_t> fake_prog(1, ax.max_progress - 1);
                ax.progress = fake_prog(rng);
            } else {
                ax.achieved = false;
                ax.unlock_time = 0;
                ax.max_progress = 0;
                ax.progress = 0;
            }
        }
        if (ach_global_percentages.find(ax.name) == ach_global_percentages.end())
            ach_global_percentages[ax.name] = global_dist(rng);
    }
}

void Steam_Overlay::Bridge_InviteAllFriends()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready() || !i_have_lobby) return;
    invite_all_friends_clicked = true;
}

void Steam_Overlay::Bridge_FriendAction(uint64_t steam_id, int action)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            switch (action) {
            case 1: // invite
                if (i_have_lobby) {
                    state.window_state |= window_state_invite;
                    has_friend_action.push(frd);
                }
                break;
            case 2: // join
                if (state.joinable) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
                break;
            case 4: // chat
                state.window_state |= window_state_show;
                break;
            case 5: // accept invite
                if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                    state.chat_history.append("[INVITE ACCEPTED] You accepted the invite\n");
                }
                break;
            case 6: // refuse invite
                if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                    state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                    state.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                }
                break;
            case 7: // kick from lobby
                {
                    CSteamID my_lobby = settings->get_lobby();
                    if (my_lobby.IsValid()) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        mm->KickLobbyMember(my_lobby.ConvertToUint64(), steam_id);
                    }
                }
                break;
            default: break;
            }
            break;
        }
    }
}

void Steam_Overlay::Bridge_AcceptLobbyJoinRequest(int notification_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == notification_id && (notification_type)n.type == notification_type::lobby_join_request) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            mm->AcceptLobbyJoinRequest(n.join_request_lobby_id, n.join_request_requester_id);
            notified_lobby_join_requests.erase({n.join_request_lobby_id, n.join_request_requester_id});
            n.start_time = {};
            n.expired = true;
            break;
        }
    }
}

void Steam_Overlay::Bridge_DeclineLobbyJoinRequest(int notification_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == notification_id && (notification_type)n.type == notification_type::lobby_join_request) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            mm->DeclineLobbyJoinRequest(n.join_request_lobby_id, n.join_request_requester_id);
            notified_lobby_join_requests.erase({n.join_request_lobby_id, n.join_request_requester_id});
            n.start_time = {};
            n.expired = true;
            break;
        }
    }
}

void Steam_Overlay::Bridge_RequestJoinFriendLobby(int notification_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == notification_id && (notification_type)n.type == notification_type::friend_lobby_available) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            PRINT_DEBUG("requesting join to lobby %" PRIu64 " (friend %" PRIu64 ")", n.join_request_lobby_id, n.source_friend_id);
            mm->JoinLobby(CSteamID((uint64)n.join_request_lobby_id));
            n.start_time = {};
            n.expired = true;
            break;
        }
    }
}

void Steam_Overlay::Bridge_KickAllLobbyMembers()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    CSteamID my_lobby = settings->get_lobby();
    if (!my_lobby.IsValid()) return;

    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
    mm->KickAllLobbyMembers(my_lobby.ConvertToUint64());
}

void Steam_Overlay::Bridge_LeaveLobby()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    CSteamID my_lobby = settings->get_lobby();
    if (!my_lobby.IsValid()) return;

    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
    mm->LeaveLobby(my_lobby);
}

void Steam_Overlay::Bridge_SetShowFps(bool v)
{
    stats.show_fps = v;
    allow_renderer_frame_processing(v);
}

void Steam_Overlay::Bridge_SetShowFrametime(bool v)
{
    stats.show_frametime = v;
    allow_renderer_frame_processing(v);
}

void Steam_Overlay::Bridge_SetShowPlaytime(bool v)
{
    stats.show_playtime = v;
    allow_renderer_frame_processing(v);
}

void Steam_Overlay::Bridge_SetShowFpsGraph(bool v)       { stats.show_fps_graph = v; }
void Steam_Overlay::Bridge_SetShowFrametimeGraph(bool v) { stats.show_frametime_graph = v; }
void Steam_Overlay::Bridge_SetShowMinMaxAvg(bool v)      { stats.show_min_max_avg = v; }
void Steam_Overlay::Bridge_SetShowPercentile1(bool v)    { stats.show_percentile_1 = v; }
void Steam_Overlay::Bridge_SetShowPercentile5(bool v)    { stats.show_percentile_5 = v; }
void Steam_Overlay::Bridge_SetShowPercentile01(bool v)   { stats.show_percentile_01 = v; }
void Steam_Overlay::Bridge_SetGraphTimeframe(int sec)    { stats.graph_timeframe_sec = (sec >= 1 && sec <= 30) ? sec : 5; }

/* ── Chat support for ReShade addon ─────────────────────────────────────── */

int Steam_Overlay::Bridge_GetChatState(uint64_t steam_id, GSE_ChatState *out)
{
    if (!out) return 0;
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            out->steam_id = steam_id;
            out->is_open = (state.window_state & window_state_show) ? 1 : 0;
            out->has_pending_invite = (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) ? 1 : 0;
            out->needs_attention = (state.window_state & window_state_need_attention) ? 1 : 0;
            
            // Copy chat history (truncate if needed)
            size_t hist_len = state.chat_history.size();
            if (hist_len >= GSE_CHAT_HISTORY_SIZE) hist_len = GSE_CHAT_HISTORY_SIZE - 1;
            memcpy(out->chat_history, state.chat_history.c_str(), hist_len);
            out->chat_history[hist_len] = '\0';
            
            // Copy window title
            strncpy(out->window_title, state.window_title.c_str(), sizeof(out->window_title) - 1);
            out->window_title[sizeof(out->window_title) - 1] = '\0';
            
            return 1;
        }
    }
    return 0;
}

void Steam_Overlay::Bridge_SendChatMessage(uint64_t steam_id, const char *msg)
{
    if (!msg || !msg[0]) return;
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            // Copy message to chat input
            strncpy(state.chat_input, msg, max_chat_len - 1);
            state.chat_input[max_chat_len - 1] = '\0';
            
            // Trigger send
            state.window_state |= window_state_send_message;
            has_friend_action.push(frd);
            return;
        }
    }
}

void Steam_Overlay::Bridge_OpenChat(uint64_t steam_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            state.window_state |= window_state_show;
            state.window_state &= ~window_state_need_attention;
            return;
        }
    }
}

void Steam_Overlay::Bridge_CloseChat(uint64_t steam_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            state.window_state &= ~window_state_show;
            return;
        }
    }
}

/* ── Avatar support for ReShade addon ───────────────────────────────────── */

int Steam_Overlay::Bridge_GetAvatar(uint64_t steam_id, GSE_AvatarData *out)
{
    if (!out) return 0;
    out->steam_id = steam_id;
    out->valid = 0;
    
    // Get avatar image handle from Steam Friends
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (!steamFriends) return 0;
    
    // Get medium avatar (64x64)
    int avatar_handle = steamFriends->GetMediumFriendAvatar(CSteamID((uint64)steam_id));
    if (avatar_handle == 0) return 0;
    
    // Get image RGBA data
    Image_Data *img = settings->get_image(avatar_handle);
    if (!img || img->data.empty()) return 0;
    
    uint32 w = img->width;
    uint32 h = img->height;
    if (w == 0 || h == 0) return 0;
    
    // Copy image data (resize if needed)
    if (w == GSE_AVATAR_SIZE && h == GSE_AVATAR_SIZE) {
        // Perfect size, direct copy
        size_t data_size = (std::min)(img->data.size(), (size_t)GSE_AVATAR_BYTES);
        memcpy(out->pixels, img->data.data(), data_size);
        out->valid = 1;
    } else {
        // Need to resize - for simplicity use nearest neighbor
        // This is a basic resize, consider using stb_image_resize for better quality
        for (int y = 0; y < GSE_AVATAR_SIZE; ++y) {
            for (int x = 0; x < GSE_AVATAR_SIZE; ++x) {
                int src_x = x * w / GSE_AVATAR_SIZE;
                int src_y = y * h / GSE_AVATAR_SIZE;
                size_t src_idx = (src_y * w + src_x) * 4;
                size_t dst_idx = (y * GSE_AVATAR_SIZE + x) * 4;
                if (src_idx + 3 < img->data.size()) {
                    out->pixels[dst_idx + 0] = (uint8_t)img->data[src_idx + 0];
                    out->pixels[dst_idx + 1] = (uint8_t)img->data[src_idx + 1];
                    out->pixels[dst_idx + 2] = (uint8_t)img->data[src_idx + 2];
                    out->pixels[dst_idx + 3] = (uint8_t)img->data[src_idx + 3];
                }
            }
        }
        out->valid = 1;
    }
    
    return out->valid;
}

int Steam_Overlay::Bridge_GetLocalAvatar(GSE_AvatarData *out)
{
    if (!out) return 0;
    
    // Get local user's steam ID
    CSteamID local_id = settings->get_local_steam_id();
    out->steam_id = local_id.IsValid() ? local_id.ConvertToUint64() : 0;
    out->valid = 0;
    
    // Get avatar image handle from Steam Friends
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (!steamFriends) return 0;
    
    // Get medium avatar for local user (64x64)
    int avatar_handle = steamFriends->GetMediumFriendAvatar(local_id);
    if (avatar_handle == 0) return 0;
    
    // Get image RGBA data
    Image_Data *img = settings->get_image(avatar_handle);
    if (!img || img->data.empty()) return 0;
    
    uint32 w = img->width;
    uint32 h = img->height;
    if (w == 0 || h == 0) return 0;
    
    // Copy/resize image data
    if (w == GSE_AVATAR_SIZE && h == GSE_AVATAR_SIZE) {
        size_t data_size = (std::min)(img->data.size(), (size_t)GSE_AVATAR_BYTES);
        memcpy(out->pixels, img->data.data(), data_size);
        out->valid = 1;
    } else {
        for (int y = 0; y < GSE_AVATAR_SIZE; ++y) {
            for (int x = 0; x < GSE_AVATAR_SIZE; ++x) {
                int src_x = x * w / GSE_AVATAR_SIZE;
                int src_y = y * h / GSE_AVATAR_SIZE;
                size_t src_idx = (src_y * w + src_x) * 4;
                size_t dst_idx = (y * GSE_AVATAR_SIZE + x) * 4;
                if (src_idx + 3 < img->data.size()) {
                    out->pixels[dst_idx + 0] = (uint8_t)img->data[src_idx + 0];
                    out->pixels[dst_idx + 1] = (uint8_t)img->data[src_idx + 1];
                    out->pixels[dst_idx + 2] = (uint8_t)img->data[src_idx + 2];
                    out->pixels[dst_idx + 3] = (uint8_t)img->data[src_idx + 3];
                }
            }
        }
        out->valid = 1;
    }
    
    return out->valid;
}

int Steam_Overlay::Bridge_GetLocalIP(char *out, int max_len) const
{
    if (!out || max_len <= 0) return 0;
    out[0] = '\0';
    if (!network) return 0;
    Networking::AdapterInfo adapters[16];
    int count = network->getAdapters(adapters, 16);
    size_t pos = 0;
    int written = 0;
    for (int i = 0; i < count && pos < (size_t)max_len - 60; ++i) {
        if (adapters[i].ip == 0) continue;
        if (pos > 0) { out[pos++] = '\n'; }
        // Format: "AdapterName: IP"
        size_t name_len = strlen(adapters[i].name);
        if (name_len > 0) {
            memcpy(out + pos, adapters[i].name, name_len);
            pos += name_len;
            out[pos++] = ':';
            out[pos++] = ' ';
        }
        char tmp[24];
        format_ip_address(adapters[i].ip, tmp, sizeof(tmp));
        size_t len = strlen(tmp);
        memcpy(out + pos, tmp, len);
        pos += len;
        ++written;
    }
    out[pos] = '\0';
    return written;
}

int Steam_Overlay::Bridge_GetNetworkInfo(GSE_NetAdapter *out, int max_adapters) const
{
    if (!out || max_adapters <= 0 || !network) return 0;
    memset(out, 0, sizeof(GSE_NetAdapter) * max_adapters);

    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));

    // Get adapter info
    Networking::AdapterInfo adapters[16];
    int adapter_count = network->getAdapters(adapters, 16);
    if (adapter_count > max_adapters) adapter_count = max_adapters;

    // Build name→friend map from friends list
    std::unordered_map<uint64, std::string> id_to_name;
    for (const auto &[frd, state] : friends) {
        id_to_name[(uint64)frd.id()] = frd.name();
    }

    // Helper to check if an IP (host byte order) falls in an adapter's subnet
    // lower/upper are stored in network byte order, so convert to host for proper range comparison
    auto ip_in_subnet = [](uint32 ip_host, const Networking::AdapterInfo &a) -> bool {
        if (a.lower == 0 && a.upper == 0) return false;
        uint32 lower_host = ntohl(a.lower);
        uint32 upper_host = ntohl(a.upper);
        return ip_host >= lower_host && ip_host <= upper_host;
    };

    uint64 local_id = settings->get_local_steam_id().ConvertToUint64();
    std::string local_name = settings->get_local_name();

    // Track which friends were assigned to at least one adapter
    std::unordered_set<uint64> assigned;

    for (int i = 0; i < adapter_count; ++i) {
        auto &a = adapters[i];
        auto &o = out[i];

        strncpy(o.name, a.name, sizeof(o.name) - 1);
        format_ip_address(a.ip, o.ip_str, sizeof(o.ip_str));

        // Build CIDR string
        uint32 net_ip = ntohl(a.lower);
        snprintf(o.subnet_str, sizeof(o.subnet_str), "%u.%u.%u.%u/%u",
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF, (net_ip >> 8) & 0xFF, net_ip & 0xFF,
            a.prefix_len);

        // Build IP range string
        uint32 upper_ip = ntohl(a.upper);
        snprintf(o.range_str, sizeof(o.range_str), "%u.%u.%u.%u - %u.%u.%u.%u",
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF, (net_ip >> 8) & 0xFF, net_ip & 0xFF,
            (upper_ip >> 24) & 0xFF, (upper_ip >> 16) & 0xFF, (upper_ip >> 8) & 0xFF, upper_ip & 0xFF);

        o.user_count = 0;

        // Add ourselves (our adapter IP)
        if (o.user_count < GSE_NET_MAX_USERS_PER_ADAPTER) {
            auto &u = o.users[o.user_count++];
            u.steam_id = local_id;
            strncpy(u.name, local_name.c_str(), sizeof(u.name) - 1);
            format_ip_address(a.ip, u.ip_str, sizeof(u.ip_str));
            u.is_self = 1;
        }

        // Add friends — check ALL known IPs per friend, not just the primary one
        for (const auto &[frd, state] : friends) {
            if (o.user_count >= GSE_NET_MAX_USERS_PER_ADAPTER) break;
            uint32 frd_ips[16];
            int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
            for (int k = 0; k < frd_ip_count; ++k) {
                if (frd_ips[k] != 0 && ip_in_subnet(frd_ips[k], a)) {
                    auto &u = o.users[o.user_count++];
                    u.steam_id = (uint64)frd.id();
                    strncpy(u.name, frd.name().c_str(), sizeof(u.name) - 1);
                    format_ip_address(frd_ips[k], u.ip_str, sizeof(u.ip_str));
                    u.is_self = 0;
                    assigned.insert((uint64)frd.id());
                    break; // one entry per friend per adapter
                }
            }
        }
    }

    // Add an "Other" adapter for friends with no IP matching any adapter subnet
    int result_count = adapter_count;
    bool need_other = false;
    for (const auto &[frd, state] : friends) {
        if (assigned.find((uint64)frd.id()) == assigned.end()) {
            uint32 frd_ip = network->getIP(CSteamID((uint64)frd.id()));
            if (frd_ip != 0) { need_other = true; break; }
        }
    }
    if (need_other && result_count < max_adapters) {
        auto &o = out[result_count];
        strncpy(o.name, "Other", sizeof(o.name) - 1);
        o.ip_str[0] = '\0';
        o.subnet_str[0] = '\0';
        o.user_count = 0;

        for (const auto &[frd, state] : friends) {
            if (o.user_count >= GSE_NET_MAX_USERS_PER_ADAPTER) break;
            if (assigned.find((uint64)frd.id()) != assigned.end()) continue;
            uint32 frd_ip = network->getIP(CSteamID((uint64)frd.id()));
            if (frd_ip != 0) {
                auto &u = o.users[o.user_count++];
                u.steam_id = (uint64)frd.id();
                strncpy(u.name, frd.name().c_str(), sizeof(u.name) - 1);
                format_ip_address(frd_ip, u.ip_str, sizeof(u.ip_str));
                u.is_self = 0;
            }
        }
        result_count++;
    }

    return result_count;
}

int Steam_Overlay::Bridge_GetLobbyChatState(GSE_LobbyChatState *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!i_have_lobby) return 0;

    CSteamID lobby = settings->get_lobby();
    if (!lobby.IsValid()) return 0;

    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
    if (!matchmaking) return 0;

    out->lobby_id = lobby.ConvertToUint64();

    // Fill member list
    int num_members = matchmaking->GetNumLobbyMembers(lobby);
    int member_write = 0;
    for (int i = 0; i < num_members && member_write < GSE_LOBBY_CHAT_MAX_MEMBERS; ++i) {
        CSteamID member_id = matchmaking->GetLobbyMemberByIndex(lobby, i);
        if (!member_id.IsValid()) continue;
        auto &m = out->members[member_write];
        m.steam_id = member_id.ConvertToUint64();

        // Get display name
        if (member_id == settings->get_local_steam_id()) {
            strncpy(m.name, settings->get_local_name(), sizeof(m.name) - 1);
        } else {
            Steam_Friends *steamFriends = get_steam_client()->steam_friends;
            const char *name = steamFriends ? steamFriends->GetFriendPersonaName(member_id) : nullptr;
            if (name && name[0])
                strncpy(m.name, name, sizeof(m.name) - 1);
            else
                snprintf(m.name, sizeof(m.name), "%llu", (unsigned long long)member_id.ConvertToUint64());
        }
        m.name[sizeof(m.name) - 1] = '\0';
        member_write++;
    }
    out->member_count = member_write;

    // Build chat history from chat_entries
    const auto &entries = matchmaking->GetChatEntries();
    size_t entry_count = entries.size();

    // Process new entries since last poll
    if (entry_count > lobby_chat_last_entry_count) {
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        for (size_t i = lobby_chat_last_entry_count; i < entry_count; ++i) {
            const auto &e = entries[i];
            if (e.lobby_id != lobby) continue;  // only show current lobby's messages

            // Get sender name
            std::string sender_name;
            if (e.user_id == settings->get_local_steam_id()) {
                sender_name = "You";
            } else {
                const char *name = steamFriends ? steamFriends->GetFriendPersonaName(e.user_id) : nullptr;
                if (name && name[0])
                    sender_name = name;
                else
                    sender_name = std::to_string(e.user_id.ConvertToUint64());
            }

            // Append to history (strip any embedded nulls from the message)
            std::string msg_text = e.message;
            while (!msg_text.empty() && msg_text.back() == '\0') msg_text.pop_back();
            if (!msg_text.empty()) {
                if (!lobby_chat_history.empty())
                    lobby_chat_history += '\n';
                lobby_chat_history += sender_name + ": " + msg_text;
            }
        }
        lobby_chat_last_entry_count = entry_count;
    }

    // Copy history to output
    size_t hist_len = lobby_chat_history.size();
    if (hist_len >= GSE_LOBBY_CHAT_HISTORY_SIZE) hist_len = GSE_LOBBY_CHAT_HISTORY_SIZE - 1;
    memcpy(out->chat_history, lobby_chat_history.c_str() + (lobby_chat_history.size() - hist_len), hist_len);
    out->chat_history[hist_len] = '\0';
    out->history_len = (int32_t)hist_len;

    return 1;
}

void Steam_Overlay::Bridge_SendLobbyChatMsg(const char *msg)
{
    if (!msg || !msg[0]) return;
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!i_have_lobby) return;

    CSteamID lobby = settings->get_lobby();
    if (!lobby.IsValid()) return;

    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
    if (!matchmaking) return;

    size_t len = strlen(msg);
    if (len > 4000) len = 4000;  // Steam's 4KB limit
    matchmaking->SendLobbyChatMsg(lobby, msg, (int)len + 1);
}

#endif
