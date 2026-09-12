# GSE Overlay — ReShade Addon

This is an alternative overlay renderer that uses [ReShade](https://reshade.me/) instead of the built-in ingame_overlay library.

## Files

| File | Description |
|------|-------------|
| `gse_overlay.addon64` | 64-bit ReShade addon |
| `gse_overlay.addon` | 32-bit ReShade addon |

ReShade's loader (`source/addon_manager.cpp`) accepts only `.addon` on **any** architecture, plus `.addon32` on 32-bit builds and `.addon64` on 64-bit builds. A plain `.dll` is **never** loaded, so the extension is what makes the addon work at all — do not rename the file to `.dll`.

## Installation

1. Install ReShade into your game (use the ReShade installer). If you install it as a Vulkan layer, note that a `RESHADE_ADDON == 1` build has "limited add-on functionality" and skips external addons entirely, so the addon will not load.
2. Copy the appropriate `.addon64` (64-bit) or `.addon` (32-bit) file into the same directory as the game executable (next to ReShade's `dxgi.dll` / `d3d11.dll` / etc.).
3. The addon automatically connects to the emu DLL (`steam_api64.dll` / `steam_api.dll`) via the GSE bridge.

## Usage

- **Shift+Tab** by default — toggles the main overlay. The combination is configurable: set `key_combo` in `configs.overlay.ini` (see the example file for the accepted syntax).
- Screenshots use `screenshot_combo` (default `f12`).
- The addon renders notifications, achievement icons, stats HUD, friends list, settings, the screenshot gallery, and the SCE asset browser — all through ReShade's ImGui integration.
- A settings tab ("GSE Overlay Settings") also appears inside ReShade's own overlay panel. Username and overlay language can be edited there, and `SaveSettings` writes them back.

## How it talks to the emu

The addon is a *client* of a C ABI exported by the emu (`overlay_experimental/overlay_bridge.h`), resolved with `GetProcAddress` and called from ReShade's render thread. Both sides check a 5-second heartbeat so that only one of them drives the overlay at a time.

Because the bridge only carries data, the addon can run with `enable_experimental_bridge=1` while `enable_experimental_overlay=0` (no renderer hook in the emu at all). In that mode there is no capture path, so the addon hides its screenshot UI; use `Bridge_IsScreenshotSupported()` to query it.

## See also

* [README.release.md](README.release.md) — general usage guide


## Requirements

- ReShade v5.0+ (API v18 or newer recommended).
- The emu DLL must be the **experimental** build (`api_experimental`) which exports the `GSE_OverlayBridge_*` functions.

## Notes

- The addon is Windows-only.
- It does **not** link against any emu internals — communication happens entirely through the C ABI bridge.
- If ReShade is not installed, this file is simply ignored by the game.
