#ifndef _KNOWN_TOOLS_H_
#define _KNOWN_TOOLS_H_

/*
 * Shared third-party overlay / tool detection.
 *
 * Before this header existed the Windows table was duplicated verbatim in
 * base.cpp and steam_client_interface_getter.cpp (93 entries each, still
 * identical when they were merged) and the Linux table lived only in base.cpp,
 * so the getter reported nothing on Linux. Both platforms and both consumers
 * now share the data and the probing here.
 *
 *   Windows: fixed module names, proxy-DLL export probes, module-name prefixes,
 *            and a generic ReShade-addon probe (module enumeration).
 *   Linux:   /proc/self/maps substring scan over bare sonames.
 *
 * Detection alone is only half the job - several of these tools cannot actually
 * coexist. The rules at the bottom of this header turn the detected set into a
 * list of documented incompatibilities that a caller can show to the user.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// System headers must stay OUTSIDE the namespace below: anything they declare
// inside it would become known_tools::CreateToolhelp32Snapshot (etc.) and be
// invisible at global scope.
#ifdef __WINDOWS__
#include <Windows.h>
#include <TlHelp32.h>
#include <cwchar>
#endif

// A recognised third-party module. `path` is only filled in when the entry was
// matched against an enumerated module (prefix / addon probe) or a maps line;
// fixed-name hits come from GetModuleHandleW and therefore have no path.
struct DetectedTool {
    std::string label;
    std::string type;
    std::string path;
};

namespace known_tools {

#ifdef __WINDOWS__

// Fixed module names, matched with GetModuleHandleW(). Each build only carries
// its own bitness, since a 64-bit process cannot host a 32-bit module.
struct Entry {
    const wchar_t* dll;
    const char* label;
    const char* type;
};

// Module-name prefixes for tools whose file name varies. These need a module
// enumeration because a fixed name cannot match, e.g. RenoDX ships
// renodx-<game>.addon64 / renodx-fpslimiter.addon64 / renodx-devkit.addon64.
struct PrefixEntry {
    const wchar_t* prefix;
    const char* label;
    const char* type;
};

// Exports that identify an injector hiding behind a proxy DLL name. ReShade
// itself uses the same ReShadeVersion trick to spot other ReShade instances.
struct ProxyExport {
    const char* export_name;
    const char* label;
    const char* type;
};

// Proxy DLL names an injector commonly masquerades as.
inline const wchar_t* const kProxyDlls[] = {
    L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"d3d10_1.dll", L"d3d10.dll", L"d3d9.dll",
    L"d3d8.dll", L"ddraw.dll", L"dinput8.dll", L"dinput.dll", L"winmm.dll",
    L"OpenGL32.dll", L"version.dll", L"dsound.dll", L"wininet.dll", L"winhttp.dll",
    L"xinput1_1.dll", L"xinput1_2.dll", L"xinput1_3.dll", L"xinput1_4.dll",
    L"xinput9_1_0.dll", L"xinputuap.dll",
    L"binkw32.dll", L"bink2w32.dll", L"binkw64.dll", L"bink2w64.dll",
    L"vorbisFile.dll", L"msacm32.dll", L"msvfw32.dll", L"xlive.dll"
};

inline const ProxyExport kProxyExports[] = {
    { "SK_GetVersionStr",    "Special K (proxy)",             "injector" },
    { "ReShadeVersion",      "ReShade (proxy)",               "post-processor" },
    { "GetASILoadLibrary",   "Ultimate ASI Loader (proxy)",   "modding" },
};

inline const Entry kEntries[] = {
    // --- injectors / post-processors ---
    #if defined(_WIN64)
    { L"SpecialK64.dll",            "Special K",            "injector" },
    { L"ReShade64.dll",             "ReShade",              "post-processor" },
    #else
    { L"SpecialK32.dll",            "Special K",            "injector" },
    { L"ReShade32.dll",             "ReShade",              "post-processor" },
    #endif
    { L"d3dcompiler_46e.dll",       "ENB Series",           "post-processor" },
    // ReShade fork that installs under the same proxy names ReShade does
    // (dxgi.dll etc.). Listed explicitly because it inherits ReShade's
    // ReShadeVersion export and would otherwise be reported as "ReShade (proxy)".
    #if defined(_WIN64)
    { L"GShade64.dll",              "GShade",               "post-processor" },
    #else
    { L"GShade32.dll",              "GShade",               "post-processor" },
    #endif
    // NVIDIA Streamline: the DLSS super-resolution / frame-generation framework.
    // Documented as incompatible with globally-injected Special K.
    { L"sl.interposer.dll",         "NVIDIA Streamline",    "upscaler" },

    // --- recording / streaming ---
    #if defined(_WIN64)
    { L"nvspcap64.dll",             "NVIDIA ShadowPlay",    "recording" },
    { L"graphics-hook64.dll",       "OBS Game Capture",     "recording" },
    { L"fraps64.dll",               "Fraps",                "recording" },
    { L"MedalHook64.dll",           "Medal.tv",             "recording" },
    { L"bdcam64.dll",               "Bandicam",             "recording" },
    { L"Action64.dll",              "Mirillis Action",      "recording" },
    { L"XSplit.Core64.dll",         "XSplit",               "recording" },
    { L"d3dgear64.dll",             "D3DGear",              "recording" },
    #else
    { L"nvspcap.dll",               "NVIDIA ShadowPlay",    "recording" },
    { L"graphics-hook32.dll",       "OBS Game Capture",     "recording" },
    { L"fraps32.dll",               "Fraps",                "recording" },
    { L"MedalHook.dll",             "Medal.tv",             "recording" },
    { L"bdcam32.dll",               "Bandicam",             "recording" },
    { L"Action.dll",                "Mirillis Action",      "recording" },
    { L"XSplit.Core.dll",           "XSplit",               "recording" },
    { L"d3dgear.dll",               "D3DGear",              "recording" },
    #endif
    { L"Streamlabs.dll",            "Streamlabs",           "recording" },

    // --- monitoring ---
    { L"RTSSHooks64.dll",           "RTSS",                 "monitoring" },
    { L"RTSSHooks.dll",             "RTSS",                 "monitoring" },
    #if defined(_WIN64)
    { L"fpshook64.dll",             "FPS Monitor",          "monitoring" },
    { L"PresentMon64.dll",          "Intel PresentMon",     "monitoring" },
    #else
    { L"fpshook.dll",               "FPS Monitor",          "monitoring" },
    { L"PresentMon32.dll",          "Intel PresentMon",     "monitoring" },
    #endif

    // --- store overlays ---
    { L"GameOverlayRenderer64.dll", "Steam Overlay",        "store overlay" },
    { L"GameOverlayRenderer.dll",   "Steam Overlay",        "store overlay" },
    { L"DiscordHook64.dll",         "Discord",              "store overlay" },
    { L"DiscordHook.dll",           "Discord",              "store overlay" },
    #if defined(_WIN64)
    { L"EOSOVH-Win64-Shipping.dll", "Epic Online Services", "store overlay" },
    { L"Galaxy64.dll",              "GOG Galaxy",           "store overlay" },
    { L"GalaxyOverlayRenderer64.dll", "GOG Galaxy",         "store overlay" },
    { L"igo64.dll",                 "EA App / Origin",      "store overlay" },
    { L"uplay_r2_loader64.dll",     "Ubisoft Connect",      "store overlay" },
    { L"upc_r2_loader64.dll",       "Ubisoft Connect",      "store overlay" },
    #else
    { L"EOSOVH-Win32-Shipping.dll", "Epic Online Services", "store overlay" },
    { L"Galaxy.dll",                "GOG Galaxy",           "store overlay" },
    { L"GalaxyOverlayRenderer.dll", "GOG Galaxy",           "store overlay" },
    { L"igo32.dll",                 "EA App / Origin",      "store overlay" },
    { L"uplay_r2_loader.dll",       "Ubisoft Connect",      "store overlay" },
    { L"upc_r2_loader.dll",         "Ubisoft Connect",      "store overlay" },
    #endif

    // --- GPU vendor software ---
    #if defined(_WIN64)
    { L"aaborern64.dll",            "AMD Adrenalin",        "gpu vendor" },
    { L"atiumd64.dll",              "AMD Display Driver",   "gpu vendor" },
    #else
    { L"aaborern.dll",              "AMD Adrenalin",        "gpu vendor" },
    { L"atiumdag.dll",              "AMD Display Driver",   "gpu vendor" },
    #endif
    { L"RadeonSoftware.dll",        "AMD Software",         "gpu vendor" },

    // --- system / platform overlays ---
    { L"GameBar.dll",               "Xbox Game Bar",        "system overlay" },
    { L"GameBarPresenceWriter.dll", "Xbox Game Bar",        "system overlay" },
    { L"SSOverlay64.dll",           "Samsung Gaming Hub",   "system overlay" },
    { L"SSOverlay.dll",             "Samsung Gaming Hub",   "system overlay" },
    { L"AcLayer.dll",               "Windows Compatibility","system overlay" },

    // --- gaming platforms / launchers ---
    { L"OWClient.dll",              "Overwolf",             "platform" },
    { L"OWExplorer.dll",            "Overwolf",             "platform" },
    #if defined(_WIN64)
    { L"ltc_game64.dll",            "Playnite",             "platform" },
    #else
    { L"ltc_game32.dll",            "Playnite",             "platform" },
    #endif

    // --- peripheral software ---
    #if defined(_WIN64)
    { L"Nahimic2OSD64.dll",         "Nahimic",              "peripheral" },
    { L"LogiOverlay64.dll",         "Logitech G Hub",       "peripheral" },
    { L"iCUEOverlay64.dll",         "Corsair iCUE",         "peripheral" },
    { L"SteelSeriesGG64.dll",       "SteelSeries GG",       "peripheral" },
    { L"RzChromaSDK64.dll",         "Razer Chroma",         "peripheral" },
    #else
    { L"Nahimic2OSD.dll",           "Nahimic",              "peripheral" },
    { L"LogiOverlay.dll",           "Logitech G Hub",       "peripheral" },
    { L"iCUEOverlay.dll",           "Corsair iCUE",         "peripheral" },
    { L"SteelSeriesGG.dll",         "SteelSeries GG",       "peripheral" },
    { L"RzChromaSDK.dll",           "Razer Chroma",         "peripheral" },
    #endif
    { L"NahimicOSD.dll",            "Nahimic",              "peripheral" },

    // --- communication ---
    #if defined(_WIN64)
    { L"mumble_ol_x64.dll",         "Mumble",               "communication" },
    { L"ts3overlay_hook_x64.dll",   "TeamSpeak",            "communication" },
    #else
    { L"mumble_ol.dll",             "Mumble",               "communication" },
    { L"ts3overlay_hook_x86.dll",   "TeamSpeak",            "communication" },
    #endif

    // --- VR ---
    { L"openvr_api.dll",            "SteamVR",              "vr" },
    // Same library as the Linux "libopenxr_loader.so" entry below - only the
    // prefix/suffix differ, so both platforms now report OpenXR. Vendors (Meta,
    // Valve, Windows MR) may rename their own copy, but the stock Khronos
    // loader, and therefore every app built against the official SDK, is this.
    { L"openxr_loader.dll",         "OpenXR",               "vr" },
    #if defined(_WIN64)
    { L"vrclient_x64.dll",          "SteamVR Client",       "vr" },
    { L"LibOVRRT64_1.dll",          "Oculus Runtime",       "vr" },
    #else
    { L"vrclient.dll",              "SteamVR Client",       "vr" },
    { L"LibOVRRT32_1.dll",          "Oculus Runtime",       "vr" },
    #endif
    { L"OculusXRPlugin.dll",        "Oculus/Meta",          "vr" },

    // --- anti-cheat (informational) ---
    #if defined(_WIN64)
    { L"EasyAntiCheat_x64.dll",     "EasyAntiCheat",        "anti-cheat" },
    { L"BEService_x64.dll",         "BattlEye",             "anti-cheat" },
    { L"BEClient_x64.dll",          "BattlEye",             "anti-cheat" },
    #else
    { L"EasyAntiCheat_x86.dll",     "EasyAntiCheat",        "anti-cheat" },
    { L"BEService_x86.dll",         "BattlEye",             "anti-cheat" },
    { L"BEClient_x86.dll",          "BattlEye",             "anti-cheat" },
    #endif
    { L"easyanticheat.dll",         "EasyAntiCheat",        "anti-cheat" },
    { L"vanguard.dll",              "Vanguard",             "anti-cheat" },

    // --- modding / script hooks ---
    { L"ScriptHookV.dll",           "ScriptHookV",          "modding" },
    { L"ScriptHookRDR2.dll",        "ScriptHookRDR2",       "modding" },
    { L"ScriptHook.dll",            "ScriptHook",           "modding" },
    { L"ScriptHookDotNet.dll",      "ScriptHookDotNet",     "modding" },
    #if defined(_WIN64)
    { L"version.dll",               "ASI Loader",           "modding" },
    #else
    { L"version.dll",               "ASI Loader",           "modding" },
    #endif

    // --- upscaling / frame generation overrides ---
    // These ship under names that look like system/GPU libs, so they are easy to
    // mistake for a driver component. OptiScaler and the DLSS->FSR mods replace
    // (or proxy) exactly these files.
    { L"nvngx.dll",                         "DLSS / upscaler override",        "upscaler" },
    { L"_nvngx.dll",                        "DLSS / upscaler override",        "upscaler" },
    { L"dlssg_to_fsr3_amd_is_better.dll",   "DLSS-FG to FSR3 mod",             "upscaler" },
    { L"amd_fidelityfx_dx12.dll",           "AMD FidelityFX override",         "upscaler" },
    { L"amd_fidelityfx_loader_dx12.dll",    "AMD FidelityFX override",         "upscaler" },
    { L"libxess.dll",                       "Intel XeSS",                      "upscaler" },
    { L"OptiScaler.dll",                    "OptiScaler",                      "upscaler" },
    { L"Lossless.dll",                      "Lossless Scaling",                "upscaler" },
};

inline const PrefixEntry kPrefixes[] = {
    // RenoDX is a ReShade addon whose file name is per-game, so it can only be
    // caught by prefix (or by the generic addon probe below).
    { L"renodx",                    "RenoDX",               "post-processor" },
    { L"optiscaler",                "OptiScaler",           "upscaler" },
    { L"lossless",                  "Lossless Scaling",     "upscaler" },
};

// --- helpers ---------------------------------------------------------------

inline bool starts_with_ci(const wchar_t* str, const wchar_t* prefix)
{
    return _wcsnicmp(str, prefix, wcslen(prefix)) == 0;
}

inline std::string narrow(const wchar_t* w)
{
    if (!w || !w[0]) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

/*
 * Collect every recognised third-party tool currently loaded in this process.
 *
 * specialk_detected / reshade_detected are optional out-flags used to drive
 * Special K coexistence handling; pass nullptr if not needed.
 */
inline void collect(std::vector<DetectedTool> &out,
                    bool *specialk_detected = nullptr,
                    bool *reshade_detected = nullptr)
{
    std::set<std::string> seen;
    auto add = [&out, &seen](const char* label, const char* type, const std::string &path) {
        if (!label || !seen.insert(label).second) return;
        DetectedTool t;
        t.label = label;
        t.type = type ? type : "";
        t.path = path;
        out.push_back(std::move(t));
    };

    // 1) Fixed module names.
    for (const auto &e : kEntries) {
        if (GetModuleHandleW(e.dll))
            add(e.label, e.type, std::string());
    }

    // 2) Injectors hiding behind a proxy DLL name.
    for (auto dll_name : kProxyDlls) {
        HMODULE hMod = GetModuleHandleW(dll_name);
        if (!hMod) continue;
        for (const auto &p : kProxyExports) {
            if (!GetProcAddress(hMod, p.export_name)) continue;
            add(p.label, p.type, std::string());
            const std::string label = p.label;
            if (specialk_detected && label.rfind("Special K", 0) == 0) *specialk_detected = true;
            if (reshade_detected && label.rfind("ReShade", 0) == 0) *reshade_detected = true;
            break;  // one identity per module
        }
    }

    // 3) Enumerate loaded modules once for the things a fixed name cannot catch:
    //    per-game file names, and ReShade addons in general (ours included).
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            // 3a) Known per-game name prefixes.
            for (const auto &p : kPrefixes) {
                if (starts_with_ci(me.szModule, p.prefix))
                    add(p.label, p.type, narrow(me.szExePath));
            }

            // 3b) ReShade addon probe. Every addon registers through
            //     ReShadeRegisterAddon and exports NAME/DESCRIPTION, so this
            //     catches addons with no table entry at all.
            if (GetProcAddress(me.hModule, "ReShadeRegisterAddon")) {
                auto *name = reinterpret_cast<const char *const *>(
                    GetProcAddress(me.hModule, "NAME"));
                if (name && *name && (*name)[0]) {
                    const std::string addon_name = *name;
                    if (addon_name.rfind("RenoDX", 0) == 0) {
                        add("RenoDX", "post-processor", narrow(me.szExePath));
                    } else {
                        const std::string label = "ReShade addon: " + addon_name;
                        add(label.c_str(), "post-processor", narrow(me.szExePath));
                    }
                }
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

#else // !__WINDOWS__

// ── Linux ──────────────────────────────────────────────────────────────────
//
// sonames are matched as substrings of /proc/self/maps lines, so an entry only
// needs the distinctive part of the name. Bitness needs no special handling:
// maps only ever lists the modules of the current process.
//
// IMPORTANT: /proc/self/maps lists mapped FILE PATHS only. Every entry must
// therefore be a fragment of a path that is actually mapped into the game
// process. A process name, a bare binary name, or the name of a helper the
// game never loads can never match. Tools that capture from outside the process
// (KMS/portal/pipewire recorders, external watchdogs) are undetectable here by
// construction and must not be listed, however well known they are, because a
// permanently-false entry reads as coverage that does not exist.

struct LinuxEntry {
    const char* lib;
    const char* label;
    const char* type;
};

inline const LinuxEntry kLinuxEntries[] = {
    // --- recording / streaming ---
    { "libobs.so",                  "OBS Studio",           "recording" },
    { "libobs-opengl.so",           "OBS OpenGL Capture",   "recording" },
    { "libobs-vulkan.so",           "OBS Vulkan Capture",   "recording" },
    // obs-vkcapture ships its own plugin name, distinct from OBS's built-in one
    { "libobs-vkcapture.so",        "OBS Vulkan Capture (obs-vkcapture)", "recording" },
    // NOTE: GPU Screen Recorder is deliberately absent. It captures through
    // gsr-kms-server / the portal from OUTSIDE the game process, and its
    // packages ship no shared object at all (Arch 6.1.1 installs only
    // usr/bin/gpu-screen-recorder, usr/bin/gsr-cli, usr/bin/gsr-kms-server,
    // usr/include/gsr/plugin.h, a systemd unit and scripts). Nothing of it can
    // ever appear in /proc/self/maps, so an entry here would never fire.

    // --- monitoring ---
    { "libMangoHud.so",             "MangoHud",             "monitoring" },
    { "libMangoHud_dlsym.so",       "MangoHud",             "monitoring" },

    // --- post-processing / overlay ---
    { "libvkbasalt.so",             "vkBasalt",             "post-processor" },
    { "libreshade.so",              "ReShade",              "post-processor" },
    // ReShade addons are dlopen()ed into the process, so unlike Windows (where a
    // module scan plus GetProcAddress("ReShadeRegisterAddon") is used) a path
    // suffix is enough. Covers RenoDX and every other ReShade addon on Linux.
    { ".addon64",                   "ReShade addon",        "post-processor" },
    { ".addon32",                   "ReShade addon",        "post-processor" },

    // --- store overlays ---
    { "gameoverlayrenderer.so",     "Steam Overlay",        "store overlay" },
    { "discord_game_sdk.so",        "Discord Game SDK",     "store overlay" },

    // --- gaming platforms ---
    { "libgamemodeauto.so",         "GameMode (Feral)",     "platform" },
    { "libgamemode.so",             "GameMode (Feral)",     "platform" },

    // --- frame limiting ---
    { "libstrangle.so",             "libstrangle",          "frame limiter" },

    // --- communication ---
    { "libmumble.so",               "Mumble",               "communication" },
    { "mumble_ol.so",               "Mumble",               "communication" },

    // --- VR ---
    { "libopenvr_api.so",           "SteamVR",              "vr" },
    { "libopenxr_loader.so",        "OpenXR",               "vr" },

    // --- anti-cheat (informational) ---
    { "libeasyanticheat.so",        "EasyAntiCheat",        "anti-cheat" },
    { "easyanticheat_x64.so",       "EasyAntiCheat",        "anti-cheat" },
    { "easyanticheat_x86.so",       "EasyAntiCheat",        "anti-cheat" },
    { "battleye_client.so",         "BattlEye",             "anti-cheat" },
    { "beclient_x64.so",            "BattlEye",             "anti-cheat" },
};

/*
 * Same contract as the Windows overload so callers stay platform-agnostic.
 * The two out-flags are Windows-only concepts; they are accepted and ignored.
 */
inline void collect(std::vector<DetectedTool> &out,
                    bool *specialk_detected = nullptr,
                    bool *reshade_detected = nullptr)
{
    (void)specialk_detected;
    (void)reshade_detected;

    std::set<std::string> seen;
    const char* home = getenv("HOME");
    const size_t home_len = home ? strlen(home) : 0;

    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;

    char line[1024]{};
    while (fgets(line, sizeof(line), maps)) {
        for (const auto &e : kLinuxEntries) {
            if (!strstr(line, e.lib)) continue;
            if (!seen.insert(e.label).second) continue;

            DetectedTool t;
            t.label = e.label;
            t.type = e.type;

            // The mapped file path is the 6th whitespace-separated field.
            const char* p = line;
            int fields = 0;
            while (*p && fields < 5) {
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;
                ++fields;
            }
            while (*p == ' ') ++p;
            if (*p == '/') {
                t.path = p;
                while (!t.path.empty() && (t.path.back() == '\n' || t.path.back() == '\r'))
                    t.path.pop_back();
                if (home && home_len > 0 && strncmp(t.path.c_str(), home, home_len) == 0)
                    t.path = "$HOME" + t.path.substr(home_len);
            }

            out.push_back(std::move(t));
        }
    }
    fclose(maps);
}

#endif // __WINDOWS__

// ── Cross-tool incompatibilities ───────────────────────────────────────────
//
// Several of these tools install under the same proxy DLL name, hook the same
// present/input chain, or are outright blocked by anti-cheat, so their presence
// together is a problem the user should hear about rather than discover as a
// launch crash.
//
// Only conflicts that the tool authors themselves document are listed, and every
// rule carries the page it came from. Nothing here is inferred from "both tools
// touch DirectX", because a false warning is worse than no warning - it teaches
// users to ignore the real ones.
//
// Matching:
//   label_*  case-insensitive SUBSTRING of a detected tool's label, so
//            "ReShade" also covers "ReShade (proxy)" and "ReShade addon".
//   type_*   exact match against DetectedTool::type.
// A rule fires when every one of its non-null conditions is met by a pair of
// DISTINCT detected tools - nothing conflicts with itself. Rules with only
// type_* conditions additionally require the two tools to have different labels,
// so a table that lists one tool under two file names cannot self-trigger.

struct Conflict {
    std::string severity;   // "note" | "warning" | "blocking"
    std::string title;
    std::string detail;
    std::string source;
    std::string tools;      // the detected labels that triggered it
};

struct ConflictRule {
    const char* label_a;
    const char* label_b;
    const char* type_a;
    const char* type_b;
    const char* severity;
    const char* title;
    const char* detail;
    const char* source;
};

inline const ConflictRule kConflictRules[] = {
    // ── specific, documented tool pairs ──
    {
        "Special K", "Streamline", nullptr, nullptr, "blocking",
        "Special K + NVIDIA Streamline",
        "Special K documents this combination as an incompatibility and raises its "
        "own pop-up. DLSS Frame Generation typically stops working. The supported "
        "fix is to replace the game's sl.interposer.dll with the modified build "
        "from Special K's wiki.",
        "https://wiki.special-k.info/Compatibility/Streamline"
    },
    {
        "Special K", "GShade", nullptr, nullptr, "warning",
        "Special K + GShade fight over the same proxy DLL name",
        "GShade links GShade64.dll into the game folder under proxy names such as "
        "dxgi.dll. Copying Special K's DLL over that link with 'Replace the file "
        "in the destination' overwrites the real GShade file inside Program Files, "
        "which then injects Special K into every other GShade game. Remove GShade's "
        "link before installing Special K locally.",
        "https://wiki.special-k.info/Compatibility/Issues"
    },
    {
        "Special K", "ReShade", nullptr, nullptr, "warning",
        "Special K + ReShade",
        "Two post-processors that both want the same proxy DLL, and Special K ships "
        "its own ReShade plugin on top. The emu already disables SK's ReShade "
        "plugin when ReShade is the local proxy; only one of them should hold the "
        "proxy DLL name.",
        "https://wiki.special-k.info/Compatibility/Issues"
    },

    // ── class-based rules ──
    {
        nullptr, nullptr, "anti-cheat", "injector", "blocking",
        "Anti-cheat present alongside an injector",
        "An injector is loaded in a process protected by anti-cheat. ReShade's own "
        "compatibility data records automatic bans (Vanguard/Valorant), bans "
        "without warning (War Thunder, Destiny 2, Overwatch, Call of Duty), and "
        "outright blocks (EasyAntiCheat, BattlEye, nProtect GameGuard, Roblox "
        "Byfron). Do not do this in an online game.",
        "https://www.pcgamingwiki.com/wiki/ReShade#Online_games_to_avoid"
    },
    {
        nullptr, nullptr, "anti-cheat", "post-processor", "blocking",
        "Anti-cheat present alongside a post-processor",
        "An overlay / post-processing injector is loaded in a process protected by "
        "anti-cheat - the highest-risk pairing for an online game. Note that some "
        "anti-cheats allowlist ReShade builds WITHOUT add-on support and block the "
        "full ones, which is exactly what this overlay requires.",
        "https://www.pcgamingwiki.com/wiki/ReShade#Online_games_to_avoid"
    },
    {
        nullptr, nullptr, "post-processor", "post-processor", "warning",
        "Two post-processors are loaded",
        "Two tools are both injecting into the render pipeline. Only one can win "
        "the proxy DLL name, and stacking two deep-hooking post-processors is a "
        "common cause of launch crashes and black or corrupted frames.",
        "https://wiki.special-k.info/Compatibility/Issues"
    },
    {
        nullptr, nullptr, "store overlay", "store overlay", "note",
        "Several store overlays are loaded at once",
        "Multiple store front-ends are drawing an overlay. Special K documents a "
        "circular XInput dependency ('Problematic XInput software detected', an "
        "infinite haptic feedback loop) that appears when several overlay-drawing "
        "clients are loaded, and is resolved by disabling the extras.",
        "https://wiki.special-k.info/Compatibility/Issues"
    },
};

// Case-insensitive substring test (ASCII).
inline bool contains_ci(const std::string &haystack, const char *needle)
{
    if (!needle || !*needle) return false;
    const size_t n = strlen(needle);
    if (haystack.size() < n) return false;
    for (size_t i = 0; i + n <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == n) return true;
    }
    return false;
}

inline bool conflict_side_matches(const DetectedTool &t, const char *label, const char *type)
{
    if (type != nullptr && t.type != type) return false;
    if (label != nullptr && !contains_ci(t.label, label)) return false;
    return true;
}

// Turns a detected-tool list into the list of documented incompatibilities.
inline void collect_conflicts(const std::vector<DetectedTool> &tools, std::vector<Conflict> &out)
{
    std::set<std::string> already_reported;

    for (const auto &r : kConflictRules) {
        // a side with neither label nor type would match anything: reject it
        if (r.label_a == nullptr && r.type_a == nullptr) continue;
        if (r.label_b == nullptr && r.type_b == nullptr) continue;
        if (r.title == nullptr || r.detail == nullptr) continue;

        // when neither side is pinned by a label, the two tools must at least
        // have different labels, otherwise one tool listed twice in a table
        // would report itself as a conflict
        const bool labels_pin_a = (r.label_a != nullptr);
        const bool labels_pin_b = (r.label_b != nullptr);

        size_t ia = tools.size(), ib = tools.size();
        for (size_t i = 0; i < tools.size() && ia == tools.size(); ++i) {
            if (!conflict_side_matches(tools[i], r.label_a, r.type_a)) continue;
            for (size_t j = 0; j < tools.size(); ++j) {
                if (i == j) continue;
                if (!conflict_side_matches(tools[j], r.label_b, r.type_b)) continue;
                if (!labels_pin_a && !labels_pin_b && tools[i].label == tools[j].label) continue;
                ia = i;
                ib = j;
                break;
            }
        }
        if (ia == tools.size() || ib == tools.size()) continue;

        if (!already_reported.insert(r.title).second) continue;

        Conflict c;
        c.severity = r.severity ? r.severity : "note";
        c.title = r.title;
        c.detail = r.detail;
        c.source = r.source ? r.source : "";
        c.tools = tools[ia].label + " + " + tools[ib].label;
        out.push_back(std::move(c));
    }
}

} // namespace known_tools

#endif // _KNOWN_TOOLS_H_
