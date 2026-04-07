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

// translation
#include "overlay/steam_overlay_translations.h"
// fonts
#include "fonts/unifont.hpp"
// builtin audio
#include "overlay/notification.h"

#define URL_WINDOW_NAME "URL Window"

// Swapchain linear-framebuffer state for sRGB decode: -1=unknown, 0=sRGB/SDR, 1=FP16/HDR (linear).
// Set by the one-shot screenshot callback in OverlayHookReady/Reset; read by srgb_decode_pixels_if_needed.
static int s_swapchain_is_linear = -1;

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
    if (settings->disable_overlay) return;

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
    if (settings->disable_overlay) return;

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
        if (is_ready && settings->overlay_appearance.image_gamma == Overlay_Appearance::SrgbDecode::Auto) {
            using RHT = InGameOverlay::RendererHookType_t;
            auto rtype = _renderer->GetRendererHookType();
            bool renderer_needs_decode = (rtype == RHT::DirectX10 || rtype == RHT::DirectX11 ||
                                          rtype == RHT::DirectX12 || rtype == RHT::Vulkan || rtype == RHT::Metal);
            if (renderer_needs_decode) {
                // Reset cached state and take a one-shot screenshot to detect the actual
                // back-buffer format. BeforeOverlay fires synchronously before OverlayProc
                // in the same frame, so s_swapchain_is_linear is set before AttachResource.
                s_swapchain_is_linear = -1;
                _renderer->SetScreenshotCallback([](InGameOverlay::ScreenshotCallbackParameter_t const* sc, void* user) {
                    using F = InGameOverlay::ScreenshotDataFormat_t;
                    s_swapchain_is_linear = (sc && sc->Format == F::R16G16B16A16_FLOAT) ? 1 : 0;
                    auto* r = static_cast<InGameOverlay::RendererHook_t*>(user);
                    r->SetScreenshotCallback(nullptr, nullptr);
                    r->TakeScreenshot(InGameOverlay::ScreenshotType_t::None);
                }, _renderer);
                _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
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
    
    bool res = fonts_atlas.Build();
    PRINT_DEBUG("created fonts atlas (result=%i)", (int)res);

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

        if (ach.icon == nullptr) {
            ach.icon = _renderer->CreateResource();
        }
        if (ach.icon_gray == nullptr) {
            ach.icon_gray = _renderer->CreateResource();
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
    notif.id = id;
    notif.type = (uint8)type;
    notif.message = msg;
    notif.frd = frd;
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

        default:
            PRINT_DEBUG("error unhandled type %i", (int)type);
        break;
    }

    return true;
}

void Steam_Overlay::add_chat_message_notification(std::string const &message)
{
    if (settings->disable_overlay_friend_notification) return;

    PRINT_DEBUG("'%s'", message.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    submit_notification(notification_type::message, message);
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
        // If we have the same appid, activate the invite/join buttons
        if (settings->get_local_game_id().AppID() == frd.appid()) {
            // user clicked on "invite to game"
            std::string translationInvite_tmp(translationInvite[current_language]);
            translationInvite_tmp.append("##PopupInviteToGame");
            if (i_have_lobby && ImGui::Button(translationInvite_tmp.c_str())) {
                close_popup = true;
                state.window_state |= window_state_invite;
                has_friend_action.push(frd);
            }
            
            // user clicked on "accept game invite"
            std::string translationJoin_tmp(translationJoin[current_language]);
            translationJoin_tmp.append("##PopupAcceptInvite");
            if (state.joinable && ImGui::Button(translationJoin_tmp.c_str())) {
                close_popup = true;
                // don't bother adding this friend if the button "invite all" was clicked
                // we will send them the invitation later in Steam_Overlay::steam_run_callback()
                if (!invite_all_friends_clicked) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
            }
        }

        if (close_popup || invite_all_friends_clicked) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Steam_Overlay::build_friend_window(Friend const& frd, friend_window_state& state)
{
    if (!(state.window_state & window_state_show))
        return;

    bool show = true;
    bool send_chat_msg = false;

    float width = ImGui::CalcTextSize("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA").x;
    
    if (state.window_state & window_state_need_attention && ImGui::IsWindowFocused()) {
        state.window_state &= ~window_state_need_attention;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2{ width, ImGui::GetFontSize()*8 + ImGui::GetFrameHeightWithSpacing()*4 },
        ImVec2{ std::numeric_limits<float>::max() , std::numeric_limits<float>::max() });

    ImGui::SetNextWindowBgAlpha(1.0f);
    // Window id is after the ###, the window title is the friend name
    std::string friend_window_id = std::move("###" + std::to_string(state.id));
    if (ImGui::Begin((state.window_title + friend_window_id).c_str(), &show)) {
        if (state.window_state & window_state_need_attention && ImGui::IsWindowFocused()) {
            state.window_state &= ~window_state_need_attention;
        }

        // Fill this with the chat box and maybe the invitation
        if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            ImGui::LabelText("##label", translationInvitedYouToJoinTheGame[current_language], frd.name().c_str(), frd.appid());
            ImGui::SameLine();
            if (ImGui::Button(translationAccept[current_language])) {
                state.window_state |= window_state_join;
                this->has_friend_action.push(frd);
            }

            ImGui::SameLine();
            if (ImGui::Button(translationRefuse[current_language])) {
                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
            }
        }

        ImGui::InputTextMultiline("##chat_history", &state.chat_history[0], state.chat_history.length(), { -1.0f, -2.0f * ImGui::GetFontSize() }, ImGuiInputTextFlags_ReadOnly);
        // TODO: Fix the layout of the chat line + send button.
        // It should be like this: chat input should fill the window size minus send button size (button size is fixed)
        // |------------------------------|
        // | /--------------------------\ |
        // | |                          | |
        // | |       chat history       | |
        // | |                          | |
        // | \--------------------------/ |
        // | [____chat line______] [send] |
        // |------------------------------|
        //
        // And it is like this
        // |------------------------------|
        // | /--------------------------\ |
        // | |                          | |
        // | |       chat history       | |
        // | |                          | |
        // | \--------------------------/ |
        // | [__chat line__] [send]       |
        // |------------------------------|
        float wnd_width = ImGui::GetContentRegionAvail().x;
        ImGuiStyle &style = ImGui::GetStyle();
        wnd_width -= ImGui::CalcTextSize(translationSend[current_language]).x + style.FramePadding.x * 2 + style.ItemSpacing.x + 1;

        uint64_t frd_id = frd.id();
        ImGui::PushID((const char *)&frd_id, (const char *)&frd_id + sizeof(frd_id));
        ImGui::PushItemWidth(wnd_width);

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
    }
    
    // User closed the friend window
    if (!show) {
        state.window_state &= ~window_state_show;
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

    const float noti_width = scrn_width * Notification::width_percent;
    const float msg_height = ImGui::CalcTextSize(
        noti.message.c_str(),
        noti.message.c_str() + noti.message.size(),
        false,
        noti_width - padding_all_sides - global_style.ItemSpacing.x
    ).y;
    float noti_height = msg_height;
    
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
        noti_height = msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::message: pos = settings->overlay_appearance.chat_msg_pos; break;
    default: PRINT_DEBUG("ERROR: unhandled notification type %i", (int)noti.type); break;
    }
    // add some y padding for niceness
    noti_height += 2 * global_style.WindowPadding.y;

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
    ImGui::SetNextWindowSize(ImVec2(noti_width, noti_height));
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

    ImGui::PushFont(font_notif);
    // Add window rounding
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, settings->overlay_appearance.notification_rounding);
   
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
        
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, settings_noti_alpha));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, get_notification_bg_rgba_safe());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 255, 255, settings_noti_alpha * 2));
       
        // some extra window flags for each notification type
        ImGuiWindowFlags extra_flags = ImGuiWindowFlags_NoFocusOnAppearing;
        switch ((notification_type)it->type) {
            // games like "Mafia Definitive Edition" will pause the entire game/scene if focus was stolen
            // be less intrusive for notifications that do not require interaction
            case notification_type::achievement_progress:
            case notification_type::achievement:
            case notification_type::auto_accept_invite:
            case notification_type::message:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::invite:
                // nothing
            break;

            default:
                PRINT_DEBUG("error unhandled flags for type %i", (int)it->type);
            break;
        }

        std::string wnd_name = "NotiPopupShow" + std::to_string(it->id);

        set_next_notification_pos({width, height}, elapsed_notif, noti_duration, *it, coords);
        if (ImGui::Begin(wnd_name.c_str(), nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | extra_flags)) {
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
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type::auto_accept_invite:
                    ImGui::TextWrapped("%s", it->message.c_str());
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

                // not effective
                case notification_type::achievement_progress:
                case notification_type::achievement:
                case notification_type::auto_accept_invite:
                case notification_type::message:
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

// Query per-display HDR state from the OS (for the UI info row, NOT used for decode decisions).
// Returns 1=HDR, 0=SDR, -1=unknown/unsupported.
static int detect_display_hdr_state()
{
#ifdef __WINDOWS__
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return -1;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO req{};
        req.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        req.header.size      = sizeof(req);
        req.header.adapterId = paths[i].targetInfo.adapterId;
        req.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&req.header) == ERROR_SUCCESS) {
            if (req.advancedColorEnabled && req.advancedColorSupported)
                return 1;
        }
    }
    return 0;
#else
    // TODO: Linux: read /sys/class/drm/card*/*/hdr_output_metadata or DRM HDR property
    return -1; // not yet implemented
#endif
}

// sRGB->linear decode before GPU upload.
// ingame_overlay always uploads textures as DXGI_FORMAT_R8G8B8A8_UNORM (no hardware sRGB decode),
// so on renderers that write to an sRGB framebuffer (DX10/11/12, Vulkan, Metal) the PNG bytes
// would be gamma-encoded a second time -> over-bright, over-saturated result.
// We pre-decode manually so the framebuffer encode is the only pass.
// DX9/OpenGL games typically use a linear framebuffer -> leave bytes as-is.
// In HDR/scRGB mode the game's swapchain is FP16 (linear) -> skip decode.
//
// Controlled by Overlay_Appearance::image_gamma (auto/on/off).
// "auto": decode for DX10/11/12/Vk/Metal when the game's swapchain is NOT FP16/HDR.
//         A one-shot screenshot on OverlayHookReady/Reset detects the actual back-buffer
//         format; it fires synchronously before OverlayProc so AttachResource sees the result.
// "on":   always decode.
// "off":  never decode.

static void srgb_decode_pixels_if_needed(InGameOverlay::RendererHook_t *renderer,
                                         const Overlay_Appearance::SrgbDecode mode,
                                         uint8_t *rgba, size_t npixels)
{
    using RHT = InGameOverlay::RendererHookType_t;
    using SD  = Overlay_Appearance::SrgbDecode;

    bool should_decode = false;
    if (mode == SD::On) {
        should_decode = true;
    } else if (mode == SD::Auto && renderer) {
        // Only decode for modern APIs that have sRGB-encoded framebuffers
        switch (renderer->GetRendererHookType()) {
            case RHT::DirectX10:
            case RHT::DirectX11:
            case RHT::DirectX12:
            case RHT::Vulkan:
            case RHT::Metal:
                should_decode = true;
                break;
            default:
                break; // DX9, OpenGL: linear framebuffer -> no decode needed
        }
        // If the game's swapchain is FP16 (HDR/scRGB) the framebuffer is linear -> skip decode.
        // s_swapchain_is_linear is set by the one-shot screenshot callback before OverlayProc.
        if (should_decode && s_swapchain_is_linear == 1)
            should_decode = false;
    }
    // SD::Off, or auto decided no -> nothing to do
    if (!should_decode) return;

    // One-time LUT: sRGB uint8 -> linear uint8
    static uint8_t lut[256];
    static bool lut_ready = false;
    if (!lut_ready) {
        for (int i = 0; i < 256; ++i) {
            float s = i / 255.0f;
            float l = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
            lut[i] = (uint8_t)(l * 255.0f + 0.5f);
        }
        lut_ready = true;
    }
    // Apply to R, G, B channels; leave A linear (correct for RGBA PNG)
    for (size_t i = 0; i < npixels; ++i, rgba += 4) {
        rgba[0] = lut[rgba[0]];
        rgba[1] = lut[rgba[1]];
        rgba[2] = lut[rgba[2]];
    }
}

bool Steam_Overlay::try_load_ach_icon(Overlay_Achievement &ach, bool achieved, bool upload_new_icon_to_gpu)
{
    if (!_renderer) return false;
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
        int icon_size = static_cast<int>(settings->overlay_appearance.icon_size);
        // Work on a mutable copy so we don't corrupt the cached image data
        std::string icon_data = image_info->data;
        srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
            (uint8_t*)icon_data.data(), (size_t)icon_size * icon_size);
        icon_rsrc->AttachResource((void*)icon_data.c_str(), icon_size, icon_size);
        
        PRINT_DEBUG("'%s' (result=%i)", ach.name.c_str(), (int)icon_rsrc->GetResourceId() != 0);
    }

    return icon_rsrc->GetResourceId() != 0;
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::overlay_render_proc()
{
    std::lock_guard lock(overlay_mutex);

    if (!Ready()) return;

    if (show_overlay) {
        render_main_window();
    }

    if (notifications.size()) {
        ImGuiIO &io = ImGui::GetIO();
        build_notifications(io.DisplaySize.x, io.DisplaySize.y);
    }

    if (stats.show_any_stats()) {
        stats.render_stats(current_language);
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
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.background_r,
            settings->overlay_appearance.background_g,
            settings->overlay_appearance.background_b,
            settings->overlay_appearance.background_a
        );
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colorSet);
        style_color_stack += 1;
    }

    if ((settings->overlay_appearance.element_r >= 0) &&
        (settings->overlay_appearance.element_g >= 0) &&
        (settings->overlay_appearance.element_b >= 0) &&
        (settings->overlay_appearance.element_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_r,
            settings->overlay_appearance.element_g,
            settings->overlay_appearance.element_b,
            settings->overlay_appearance.element_a
        );
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
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_hovered_r,
            settings->overlay_appearance.element_hovered_g,
            settings->overlay_appearance.element_hovered_b,
            settings->overlay_appearance.element_hovered_a
        );
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
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_active_r,
            settings->overlay_appearance.element_active_g,
            settings->overlay_appearance.element_active_b,
            settings->overlay_appearance.element_active_a
        );
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

    ImGui::PushFont(font_default);
    uint32 style_color_stack = apply_global_style_color();

    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ io.DisplaySize.x, io.DisplaySize.y });
    if (ImGui::Begin(windowTitle.c_str(), &show,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        if (show_user_info) {
            ImGui::LabelText("##playinglabel", translationUserPlaying[current_language],
                settings->get_local_name(),
                settings->get_local_steam_id().ConvertToUint64(),
                settings->get_local_game_id().AppID());
        }

        ImGui::Spacing();
        
        ImGui::SameLine();
        // user clicked on "toggle user info"
        if (ImGui::Button(translationToggleUserInfo[current_language])) {
            show_user_info = !show_user_info;
        }

        ImGui::SameLine();
        // user clicked on "show achievements"
        if (ImGui::Button(translationShowAchievements[current_language])) {
            show_achievements = !show_achievements;
        }

        ImGui::SameLine();
        // user clicked on "test achievement"
        if (ImGui::Button(translationTestAchievement[current_language])) {
            show_test_achievement();
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
        // user clicked on "FPS"
        ImGui::SameLine();
        if (ImGui::Checkbox(translationFpsCheckbox[current_language], &stats.show_fps)) {
            allow_renderer_frame_processing(stats.show_fps);
        }
        // user clicked on "Frametime"
        ImGui::SameLine();
        if (ImGui::Checkbox(translationFrametimeCheckbox[current_language], &stats.show_frametime)) {
            allow_renderer_frame_processing(stats.show_frametime);
        }
        // user clicked on "Playtime"
        ImGui::SameLine();
        if (ImGui::Checkbox(translationPlaytimeCheckbox[current_language], &stats.show_playtime)) {
            allow_renderer_frame_processing(stats.show_playtime);
        }

        // --- Renderer / HDR status info row ----------------------------
        ImGui::Spacing();
        {
            using RHT = InGameOverlay::RendererHookType_t;
            const char* renderer_lib = _renderer ? _renderer->GetLibraryName() : "None";

            // Game swapchain color-space (derived from the screenshot-callback format)
            const char* game_fmt;
            if (!_renderer) {
                game_fmt = "N/A";
            } else {
                auto rtype = _renderer->GetRendererHookType();
                bool hdr_api = (rtype == RHT::DirectX10 || rtype == RHT::DirectX11 ||
                                rtype == RHT::DirectX12 || rtype == RHT::Vulkan || rtype == RHT::Metal);
                if (!hdr_api) {
                    game_fmt = "SDR (linear)";
                } else if (s_swapchain_is_linear == 1) {
                    game_fmt = "HDR (FP16)";
                } else if (s_swapchain_is_linear == 0) {
                    game_fmt = "SDR (sRGB)";
                } else {
                    game_fmt = "Detecting...";
                }
            }

            // Display HDR state — queried once per launch; user can refresh via button.
            static int display_hdr_state = -2; // -2=not yet queried, -1=unsupported, 0=SDR, 1=HDR
            if (display_hdr_state == -2)
                display_hdr_state = detect_display_hdr_state();

            const char* display_hdr_str;
            if      (display_hdr_state == 1)  display_hdr_str = "HDR";
            else if (display_hdr_state == 0)  display_hdr_str = "SDR";
            else                              display_hdr_str = "N/A";

            ImGui::TextDisabled("Renderer: %s  |  Game: %s  |  Display: %s",
                renderer_lib, game_fmt, display_hdr_str);
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh##hdr_info")) {
                display_hdr_state = detect_display_hdr_state();
                s_swapchain_is_linear = -1; // re-arm the screenshot callback on next Ready
                if (_renderer)
                    _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
            }
        }
        // ---------------------------------------------------------------

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::LabelText("##label", "%s", translationFriends[current_language]);

        if (!friends.empty()) {
            if (i_have_lobby) {
                std::string inviteAll(translationInviteAll[current_language]);
                inviteAll.append("##PopupInviteAllFriends");
                if (ImGui::Button(inviteAll.c_str())) { // if btn clicked
                    invite_all_friends_clicked = true;
                }
            }

            if (ImGuiHelper_BeginListBox("##label", static_cast<int>(friends.size()))) {
                std::for_each(friends.begin(), friends.end(), [this](std::pair<Friend const, friend_window_state> &i) {
                    ImGui::PushID(i.second.id-base_friend_window_id+base_friend_item_id);

                    ImGui::Selectable(i.second.window_title.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                    build_friend_context_menu(i.first, i.second);
                    if (ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0)) {
                        i.second.window_state |= window_state_show;
                    }

                    ImGui::PopID();

                    build_friend_window(i.first, i.second);
                });
                ImGui::EndListBox();
            }
        }

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
                            // regular achievement: ~50% chance achieved
                            if (roll < 50) {
                                ax.achieved = true;
                                ax.unlock_time = now_ts - time_dist(rng);
                            } else {
                                ax.achieved = false;
                                ax.unlock_time = 0;
                            }
                        }

                        // simulate a global percentage only if we don't have a real one
                        if (ach_global_percentages.find(ax.name) == ach_global_percentages.end())
                            ach_global_percentages[ax.name] = global_dist(rng);
                    }
                }
                ImGui::BeginChild(translationAchievements[current_language]);

                // ---- sort / group controls ----
                Steam_User_Stats *steamUserStats_sh = get_steam_client()->steam_user_stats;
                bool has_sh_groups = steamUserStats_sh->steamhunters_data_populated
                                     && !steamUserStats_sh->steamhunters_achievement_groups.empty();

                if (has_sh_groups) {
                    if (ImGui::Button(ach_group_by_sh ? "Ungroup##ach_grp" : "Group by DLC##ach_grp"))
                        ach_group_by_sh = !ach_group_by_sh;
                    ImGui::SameLine();
                }
                if (ImGui::Button(ach_sort_schema_order ? "Sort: Schema Order##ach_srt" : "Sort: Global %%##ach_srt"))
                    ach_sort_schema_order = !ach_sort_schema_order;

                ImGui::Separator();

                // ---- comparator (unlocked-recent → locked → hidden; schema or global % for locked) ----
                auto ach_compare = [&](size_t ai, size_t bi) -> bool {
                    const auto &a = achievements[ai];
                    const auto &b = achievements[bi];
                    bool a_hidden = a.hidden && !a.achieved;
                    bool b_hidden = b.hidden && !b.achieved;
                    if (a_hidden != b_hidden) return !a_hidden;
                    if (a_hidden) return false;
                    if (a.achieved != b.achieved) return a.achieved > b.achieved;
                    if (a.achieved) return a.unlock_time > b.unlock_time;
                    if (!ach_sort_schema_order) {
                        auto ita = ach_global_percentages.find(a.name);
                        auto itb = ach_global_percentages.find(b.name);
                        float pa = (ita != ach_global_percentages.end()) ? ita->second : -1.0f;
                        float pb = (itb != ach_global_percentages.end()) ? itb->second : -1.0f;
                        if (pa != pb) return pa > pb;
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

                    if (has_icon) {
                        {
                            const char *sym_for_measure = achieved ? u8"\u2713" : u8"\u2717";
                            float sym_w = ImGui::CalcTextSize(sym_for_measure).x;
                            float title_x_offset = (icon_col_w - sym_w) * 0.5f;
                            if (title_x_offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + title_x_offset);
                        }
                        if (hidden) ImGui::Text("%s", translationHiddenAchievement[current_language]);
                        else        ImGui::Text("%s", x.title.c_str());

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
                            ImGui::Text("%s", translationHiddenAchievement[current_language]);
                            ImGui::TextDisabled("%s", x.description.c_str());
                        } else {
                            ImGui::Text("%s", x.title.c_str());
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
                        if (has_progress) snprintf(pbuf, sizeof(pbuf), "%u/%u", x.progress, x.max_progress);

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

                        if (has_progress) {
                            ImVec2 pbar_sz  = ImGui::CalcTextSize(pbuf);
                            ImVec2 pbar_pos = { bar_pos.x + (bar_width - pbar_sz.x) * 0.5f, bar_pos.y + (bar_h - pbar_sz.y) * 0.5f };
                            draw_shadowed(pbar_pos, IM_COL32(255, 255, 255, 255), pbuf);
                        }
                    }

                    // --- global % ---
                    {
                        auto it = ach_global_percentages.find(x.name);
                        if (it != ach_global_percentages.end())
                            ImGui::TextDisabled(translationGlobalAchievementPercent[current_language], it->second);
                    }

                    ImGui::Separator();
                }; // end render_ach_item

                // ---- build name → index map (needed for grouping) ----
                std::unordered_map<std::string, size_t> ach_name_idx;
                ach_name_idx.reserve(achievements.size());
                for (size_t i = 0; i < achievements.size(); ++i)
                    ach_name_idx[achievements[i].name] = i;

                if (ach_group_by_sh && has_sh_groups) {
                    // === GROUPED RENDER ===
                    std::unordered_set<size_t> rendered_set;

                    for (const auto &grp : steamUserStats_sh->steamhunters_achievement_groups) {
                        std::vector<size_t> grp_idx;
                        for (const auto &api_name : grp.achievementApiNames) {
                            auto it = ach_name_idx.find(api_name);
                            if (it != ach_name_idx.end()) {
                                grp_idx.push_back(it->second);
                                rendered_set.insert(it->second);
                            }
                        }
                        if (grp_idx.empty()) continue;
                        std::stable_sort(grp_idx.begin(), grp_idx.end(), ach_compare);

                        // group header: "DLC Name" or "DLC Name — Sub-group"
                        std::string hdr = grp.dlcAppName.empty() ? "Base Game" : grp.dlcAppName;
                        if (!grp.name.empty()) hdr += " \xe2\x80\x94 " + grp.name; // em-dash

                        // collapsible tree node; default open
                        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.20f, 0.30f, 0.45f, 0.80f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.25f, 0.38f, 0.55f, 0.90f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.30f, 0.45f, 0.65f, 1.00f));
                        bool open = ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (size_t gi : grp_idx) render_ach_item(achievements[gi]);
                        }
                    }

                    // render any achievements not assigned to any group as "Base Game"
                    std::vector<size_t> ungrouped;
                    for (size_t i = 0; i < achievements.size(); ++i)
                        if (rendered_set.find(i) == rendered_set.end()) ungrouped.push_back(i);
                    if (!ungrouped.empty()) {
                        std::stable_sort(ungrouped.begin(), ungrouped.end(), ach_compare);
                        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.20f, 0.30f, 0.45f, 0.80f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.25f, 0.38f, 0.55f, 0.90f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.30f, 0.45f, 0.65f, 1.00f));
                        bool open = ImGui::CollapsingHeader("Base Game##ach_base", ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (size_t gi : ungrouped) render_ach_item(achievements[gi]);
                        }
                    }
                } else {
                    // === FLAT SORTED RENDER ===
                    std::vector<size_t> sorted_idx(achievements.size());
                    std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
                    std::stable_sort(sorted_idx.begin(), sorted_idx.end(), ach_compare);
                    for (size_t si : sorted_idx) render_ach_item(achievements[si]);
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
                sce_textures_free_all();
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

                // Free textures when the window is closed
                if (!browser_open) {
                    show_sce_browser = false;
                    sce_preview_key.clear();
                    sce_preview_nav_keys.clear();
                    sce_textures_free_all();
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
    if (settings->disable_overlay) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    bool not_called_yet = false;
    if (setup_overlay_called.compare_exchange_weak(not_called_yet, true)) {
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
    if (settings->disable_overlay) return;

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
    return !settings->disable_overlay && is_ready && late_init_imgui;
}

bool Steam_Overlay::NeedPresent() const
{
    PRINT_DEBUG_ENTRY();
    return !settings->disable_overlay;
}

void Steam_Overlay::SetNotificationPosition(ENotificationPosition eNotificationPosition)
{
    if (settings->disable_overlay) return;

    PRINT_DEBUG("TODO %i", (int)eNotificationPosition);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    notif_position = eNotificationPosition;
}

void Steam_Overlay::SetNotificationInset(int nHorizontalInset, int nVerticalInset)
{
    if (settings->disable_overlay) return;

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
        auto& frd = i->second;
        frd.lobbyId = lobbyId;
        frd.window_state |= window_state_lobby_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_rich_invite;
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
        auto& frd = i->second;
        strncpy(frd.connect, connect_str, k_cchMaxRichPresenceValueLength - 1);
        frd.window_state |= window_state_rich_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_lobby_invite;
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::FriendConnect(Friend _friend)
{
    if (settings->disable_overlay) return;

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
    if (settings->disable_overlay) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;
    
    auto it = friends.find(_friend);
    if (it != friends.end())
        friends.erase(it);
}

// show a notification when the user unlocks an achievement
void Steam_Overlay::AddAchievementNotification(const std::string &ach_name, nlohmann::json const &ach, bool for_progress)
{
    if (settings->disable_overlay) return;

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
    if (std::string(steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect")).length() > 0) {
        i_have_lobby = true;
    } else if (settings->get_lobby().IsValid()) {
        i_have_lobby = true;
    } else {
        i_have_lobby = false;
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
    if (friend_game_info.m_steamIDLobby.IsValid() && (f.second.window_state & window_state_lobby_invite)) {
        PRINT_DEBUG("%" PRIu64 " true (friend in a game)", f.first.id());
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

    std::for_each(friends.begin(), friends.end(), [this](std::pair<Friend const, friend_window_state> &i) {
        i.second.joinable = is_friend_joinable(i);
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
                    network->sendTo(&msg, true);

                    friend_info->second.chat_history.append(get_steam_client()->settings_client->get_local_name()).append(": ").append(input).append("\n", 1);
                }
                *input = 0; // Reset the input field

                friend_info->second.window_state &= ~window_state_send_message;
            }
            // The user clicked on "Invite" (but invite all wasn't clicked)
            if (friend_info->second.window_state & window_state_invite) {
                invite_friend(friend_id, steamFriends, steamMatchmaking);
                
                friend_info->second.window_state &= ~window_state_invite;
            }
            // The user clicked on "Join"
            if (friend_info->second.window_state & window_state_join) {
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
                
                friend_info->second.window_state &= ~window_state_join;
            }
        }
        has_friend_action.pop();
    }

}

void Steam_Overlay::steam_run_callback()
{
    if (!Ready()) return;

    if (overlay_state_changed) {
        overlay_state_changed = false;

        GameOverlayActivated_t data{};
        data.m_bActive = show_overlay;
        data.m_bUserInitiated = true;
        data.m_dwOverlayPID = 123;
        data.m_nAppID = settings->get_local_game_id().AppID();
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
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

    steam_run_callback_update_my_lobby();

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
            if (!(friend_info->second.window_state & window_state_show)) {
                friend_info->second.window_state |= window_state_need_attention;
            }

            add_chat_message_notification(friend_info->first.name() + ": " + steam_message.message());
            notify_sound_user_invite(friend_info->second);
        }
    }
}


#endif
