# Steam Stats Converter

A Python 3 tool that converts achievements and stats between **Steam's binary
`UserGameStats` format** and the **Gold Steam Emulator (GSE) JSON/binary format**.

Requires Python 3.10 or newer (standard library only — no extra packages needed).

---

## Background

### Steam's binary format  (`appcache/stats/`)

Steam caches two files per game in `<Steam root>/appcache/stats/`:

| File | Purpose |
|------|---------|
| `UserGameStatsSchema_<appid>.bin` | Achievement & stat **definitions** — names, types, bit-positions |
| `UserGameStats_<steamid>_<appid>.bin` | Per-user **earned data** — which achievements are unlocked, timestamps, stat values |

Both files use Valve's **binary KeyValues (VDF)** format.  
Achievement groups store a 32-bit bitmask (`data`) plus an `AchievementTimes` sub-dict
(`bit_index -> unix_timestamp`).  
Stat groups store the raw int32/float32 value in `data`.

### GSE format  (`steam_settings/` + save folder)

| File | Purpose |
|------|---------|
| `steam_settings/achievements.json` | Achievement definitions (name, displayName, description, icons) |
| `steam_settings/stats.json` | Stat definitions (name, type, default) |
| `<save>/<appid>/achievements.json` | User earned data: `{"ACH_NAME": {"earned": true, "earned_time": 1234567890}}` |
| `<save>/<appid>/stats/<stat_name>` | Raw 4-byte int32 (INT) or float32 (FLOAT/AVGRATE) per stat |

---

## Usage

### 1 — Inspect a schema file

```
python steam_stats_converter.py dump-schema --appid 1234
# or
python steam_stats_converter.py dump-schema --schema UserGameStatsSchema_1234.bin
```

Lists every achievement API name and stat name with their group/bit positions.

---

### 2 — Steam → GSE  (`steam2gse`)

Reads the Steam Schema + UserGameStats files and writes GSE-compatible save files.

```bash
# Fully automatic (auto-detects your Steam installation and SteamID):
python steam_stats_converter.py steam2gse --appid 1234 --out-dir ./gse_save

# With explicit paths:
python steam_stats_converter.py steam2gse \
    --schema  "C:\Steam\appcache\stats\UserGameStatsSchema_1234.bin" \
    --ugs     "C:\Steam\appcache\stats\UserGameStats_76561198012345678_1234.bin" \
    --out-dir ./gse_save
```

**Output:**
```
gse_save/
  achievements.json     <- GSE user achievements (earned + timestamps)
  stats/
    stat_name_1         <- 4-byte little-endian int32 or float32
    stat_name_2
    ...
```

Place `achievements.json` and `stats/` into:
```
<GSE save folder>/<appid>/
```
The default GSE save folder on Windows is:
```
%APPDATA%\GSE Saves\<appid>\
```

---

### 3 — GSE → Steam  (`gse2steam`)

Reads GSE save files and writes a Steam `UserGameStats_<steamid>_<appid>.bin`.

```bash
# Automatic SteamID detection:
python steam_stats_converter.py gse2steam \
    --appid     1234 \
    --ach-json  ./gse_save/achievements.json \
    --stats-dir ./gse_save/stats

# Explicit SteamID:
python steam_stats_converter.py gse2steam \
    --appid     1234 \
    --steamid   76561198012345678 \
    --ach-json  ./gse_save/achievements.json \
    --stats-dir ./gse_save/stats \
    --out       UserGameStats_76561198012345678_1234.bin
```

**Output:** A `.bin` file ready to be placed in:
```
<Steam root>/appcache/stats/UserGameStats_<steamid>_<appid>.bin
```

> **Note:** The CRC field in the output is set to 0. Steam recomputes and
> overwrites it on the next sync. All achievement and stat data is fully
> preserved.

---

## Common options

| Flag | Description |
|------|-------------|
| `--schema` | Explicit path to `UserGameStatsSchema_<appid>.bin` |
| `--appid` | App ID — used for auto-detection of schema/UGS files |
| `--steamid` | 64-bit SteamID or `auto` (default) |
| `--verbose` | Print every achievement/stat as it is converted |

---

## File locations

### Windows

```
Steam root:  C:\Program Files (x86)\Steam
Schema:      <Steam root>\appcache\stats\UserGameStatsSchema_<appid>.bin
User data:   <Steam root>\appcache\stats\UserGameStats_<steamid3>_<appid>.bin
```

> The filename uses the **steamid3** form (not the 64-bit steamid64).  
> steamid64 = 0x110000100000000 + steamid3

### Linux / macOS

```
~/.steam/steam/appcache/stats/
~/.local/share/Steam/appcache/stats/
```

---

## Round-trip accuracy

Tested on multiple games — the output binary is **byte-for-byte identical** to
the original Steam file, except for the 4-byte CRC field (offsets 12–15 in the
`cache` root), which Steam recomputes on next sync.
