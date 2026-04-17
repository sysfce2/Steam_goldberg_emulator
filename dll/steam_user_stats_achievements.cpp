/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/steam_user_stats.h"
#include <curl/curl.h>
#include <ctime>
#include <random>


// --- achievement_trigger ---
bool achievement_trigger::should_unlock_ach(float stat) const
{
    try {
        if (std::stof(max_value) <= stat) return true;
    } catch (...) {}

    return false;
}

bool achievement_trigger::should_unlock_ach(int32 stat) const
{
    try {
        if (std::stoi(max_value) <= stat) return true;
    } catch (...) {}

    return false;
}

bool achievement_trigger::should_indicate_progress(float stat) const
{
    // show progress if number < max
    try {
        if (std::stof(max_value) > stat) return true;
    } catch (...) {}

    return false;
}

bool achievement_trigger::should_indicate_progress(int32 stat) const
{
    // show progress if number < max
    try {
        if (std::stoi(max_value) > stat) return true;
    } catch (...) {}

    return false;
}
// --- achievement_trigger ---



void Steam_User_Stats::load_achievements_db()
{
    std::string file_path = Local_Storage::get_game_settings_path() + achievements_user_file;
    local_storage->load_json(file_path, defined_achievements);
}

void Steam_User_Stats::load_achievements()
{
    local_storage->load_json_file("", achievements_user_file, user_achievements);
}

void Steam_User_Stats::save_achievements()
{
    local_storage->write_json_file("", achievements_user_file, user_achievements);
}

int Steam_User_Stats::load_ach_icon(nlohmann::json &defined_ach, bool achieved)
{
    const char *icon_handle_key = achieved ? "icon_handle" : "icon_gray_handle";
    int current_handle = defined_ach.value(icon_handle_key, Settings::UNLOADED_IMAGE_HANDLE);
    if (Settings::UNLOADED_IMAGE_HANDLE != current_handle) { // already loaded
        return current_handle;
    }

    const char *icon_key = achieved ? "icon" : "icon_gray";
    if (!achieved && !defined_ach.contains(icon_key)) {
        icon_key = "icongray"; // old format
    }

    std::string icon_filepath = defined_ach.value(icon_key, std::string{});
    if (icon_filepath.empty()) {
        defined_ach[icon_handle_key] = Settings::INVALID_IMAGE_HANDLE;
        return Settings::INVALID_IMAGE_HANDLE;
    }

    std::string file_path(Local_Storage::get_game_settings_path() + icon_filepath);
    unsigned int file_size = file_size_(file_path);
    if (!file_size)
    {
        file_path = (Local_Storage::get_game_settings_path() + "achievement_images" + PATH_SEPARATOR + icon_filepath);
        file_size = file_size_(file_path);
        if (!file_size)
        {
            defined_ach[icon_handle_key] = Settings::INVALID_IMAGE_HANDLE;
            return Settings::INVALID_IMAGE_HANDLE;
        }
    }

    int icon_size = static_cast<int>(settings->overlay_appearance.icon_size);
    std::string img(Local_Storage::load_image_resized(file_path, "", icon_size));
    if (img.empty()) {
        defined_ach[icon_handle_key] = Settings::INVALID_IMAGE_HANDLE;
        return Settings::INVALID_IMAGE_HANDLE;
    }

    int handle = settings->add_image(img, icon_size, icon_size);
    defined_ach[icon_handle_key] = handle;
    return handle;
}

nlohmann::detail::iter_impl<nlohmann::json> Steam_User_Stats::defined_achievements_find(const std::string &key)
{
    return std::find_if(
        defined_achievements.begin(), defined_achievements.end(),
        [&key](const nlohmann::json& item) {
            const std::string &name = static_cast<const std::string &>( item.value("name", std::string()) );
            return common_helpers::str_cmp_insensitive(key, name);
        }
    );
}

std::string Steam_User_Stats::get_value_for_language(const nlohmann::json &json, std::string_view key, std::string_view language)
{
    auto x = json.find(key); // find "displayName", or "description", etc ...
    if (json.end() == x) return "";

    if (x.value().is_string()) { // ex: "displayName": "some description"
        return x.value().get<std::string>();
    } else if (x.value().is_object()) {
        const auto &obj_kv_pairs = x.value().items();

        // try to find target language
        auto obj_itr = std::find_if(obj_kv_pairs.begin(), obj_kv_pairs.end(), [&]( decltype(*obj_kv_pairs.begin()) item ) {
            return common_helpers::str_cmp_insensitive(item.key(), language);
        });
        if (obj_itr != obj_kv_pairs.end()) {
            return obj_itr.value().get<std::string>();
        }

        // try to find english language
        obj_itr = std::find_if(obj_kv_pairs.begin(), obj_kv_pairs.end(), [&]( decltype(*obj_kv_pairs.begin()) item ) {
            return common_helpers::str_cmp_insensitive(item.key(), "english");
        });
        if (obj_itr != obj_kv_pairs.end()) {
            return obj_itr.value().get<std::string>();
        }

        // try to find the first available language (not "token"),
        // if not languages exist, try to find "token"
        for (bool search_for_token : { false, true }) {
            obj_itr = std::find_if(obj_kv_pairs.begin(), obj_kv_pairs.end(), [&]( decltype(*obj_kv_pairs.begin()) item ) {
                return common_helpers::str_cmp_insensitive(item.key(), "token") == search_for_token;
            });
            if (obj_itr != obj_kv_pairs.end()) {
                return obj_itr.value().get<std::string>();
            }
        }
    }

    return "";
}



// change achievements without sending back to server
Steam_User_Stats::InternalSetResult<bool> Steam_User_Stats::set_achievement_internal( const char *pchName )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_User_Stats::InternalSetResult<bool> result{};

    if (!pchName) return result;
    
    std::string org_name(pchName);

    if (settings->achievement_bypass) {
        auto &trig = store_stats_trigger[common_helpers::to_lower(org_name)];
        trig.m_bGroupAchievement = false;
        trig.m_nCurProgress = 0;
        trig.m_nGameID = settings->get_local_game_id().ToUint64();
        trig.m_nMaxProgress = 0;
        memset(trig.m_rgchAchievementName, 0, sizeof(trig.m_rgchAchievementName));
        org_name.copy(trig.m_rgchAchievementName, sizeof(trig.m_rgchAchievementName) - 1);

        result.success = true;
        return result;
    }

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(org_name);
    } catch(...) { }
    if (defined_achievements.end() == it) return result;

    result.current_val = true;
    result.internal_name = org_name;
    result.success = true;

    try {
        std::string internal_name = it->value("name", std::string());

        result.internal_name = internal_name;

        auto ach = user_achievements.find(internal_name);
        if (user_achievements.end() == ach || ach->value("earned", false) == false) {
            user_achievements[internal_name]["earned"] = true;
            user_achievements[internal_name]["earned_time"] =
                std::chrono::duration_cast<std::chrono::duration<uint32>>(std::chrono::system_clock::now().time_since_epoch()).count();

            save_achievements();

            result.notify_server = !settings->disable_sharing_stats_with_gameserver;

            overlay->AddAchievementNotification(internal_name, user_achievements[internal_name], false);

        }
    } catch (...) {}

    auto &trig = store_stats_trigger[common_helpers::to_lower(org_name)];
    trig.m_bGroupAchievement = false;
    trig.m_nCurProgress = 0;
    trig.m_nGameID = settings->get_local_game_id().ToUint64();
    trig.m_nMaxProgress = 0;
    memset(trig.m_rgchAchievementName, 0, sizeof(trig.m_rgchAchievementName));
    org_name.copy(trig.m_rgchAchievementName, sizeof(trig.m_rgchAchievementName) - 1);

    return result;
}

Steam_User_Stats::InternalSetResult<bool> Steam_User_Stats::clear_achievement_internal( const char *pchName )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_User_Stats::InternalSetResult<bool> result{};

    if (!pchName) return result;

    std::string org_name(pchName);

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(org_name);
    } catch(...) { }
    if (defined_achievements.end() == it) return result;

    result.current_val = false;
    result.internal_name = org_name;
    result.success = true;

    try {
        std::string internal_name = it->value("name", std::string());

        result.internal_name = internal_name;

        auto ach = user_achievements.find(internal_name);
        // assume "earned" is true in case the json obj exists, but the key is absent
        // assume "earned_time" is UINT32_MAX in case the json obj exists, but the key is absent
        if (user_achievements.end() == ach ||
            ach->value("earned", true) == true ||
            ach->value("earned_time", static_cast<uint32>(UINT32_MAX)) == UINT32_MAX) {
            
            user_achievements[internal_name]["earned"] = false;
            user_achievements[internal_name]["earned_time"] = static_cast<uint32>(0);
            save_achievements();

            result.notify_server = !settings->disable_sharing_stats_with_gameserver;

            overlay->AddAchievementNotification(internal_name, user_achievements[internal_name], false);

        }
    } catch (...) {}

    store_stats_trigger.erase(common_helpers::to_lower(org_name));
    
    return result;
}


// Achievement flag accessors
bool Steam_User_Stats::GetAchievement( const char *pchName, bool *pbAchieved )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (!pchName) return false;

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(pchName);
    } catch(...) { }
    if (defined_achievements.end() == it) return false;

    // according to docs, the function returns true if the achievement was found,
    // regardless achieved or not 
    if (!pbAchieved) return true;

    *pbAchieved = false;
    try {
        std::string pch_name = it->value("name", std::string());
        auto ach = user_achievements.find(pch_name);
        if (user_achievements.end() != ach) {
            *pbAchieved = ach->value("earned", false);
        }
    } catch (...) { }

    return true;
}

bool Steam_User_Stats::SetAchievement( const char *pchName )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    auto ret = set_achievement_internal(pchName);
    if (ret.success && ret.notify_server) {
        auto &new_ach = (*pending_server_updates.mutable_user_achievements())[ret.internal_name];
        new_ach.set_achieved(ret.current_val);

        if (settings->immediate_gameserver_stats) send_updated_stats();
    }
    
    return ret.success;
}

bool Steam_User_Stats::ClearAchievement( const char *pchName )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    auto ret = clear_achievement_internal(pchName);
    if (ret.success && ret.notify_server) {
        auto &new_ach = (*pending_server_updates.mutable_user_achievements())[ret.internal_name];
        new_ach.set_achieved(ret.current_val);

        if (settings->immediate_gameserver_stats) send_updated_stats();
    }
    
    return ret.success;
}


// Get the achievement status, and the time it was unlocked if unlocked.
// If the return value is true, but the unlock time is zero, that means it was unlocked before Steam 
// began tracking achievement unlock times (December 2009). Time is seconds since January 1, 1970.
bool Steam_User_Stats::GetAchievementAndUnlockTime( const char *pchName, bool *pbAchieved, uint32 *punUnlockTime )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (!pchName) return false;

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(pchName);
    } catch(...) { }
    if (defined_achievements.end() == it) return false;

    if (pbAchieved) *pbAchieved = false;
    if (punUnlockTime) *punUnlockTime = 0;
    
    try {
        std::string pch_name = it->value("name", std::string());
        auto ach = user_achievements.find(pch_name);
        if (user_achievements.end() != ach) {
            if (pbAchieved) *pbAchieved = ach->value("earned", false);
            if (punUnlockTime) *punUnlockTime = ach->value("earned_time", static_cast<uint32>(0));
        }
    } catch (...) {}

    return true;
}


// Achievement / GroupAchievement metadata

// Gets the icon of the achievement, which is a handle to be used in ISteamUtils::GetImageRGBA(), or 0 if none set. 
// A return value of 0 may indicate we are still fetching data, and you can wait for the UserAchievementIconFetched_t callback
// which will notify you when the bits are ready. If the callback still returns zero, then there is no image set for the
// specified achievement.
int Steam_User_Stats::GetAchievementIcon( const char *pchName )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (!pchName) return Settings::INVALID_IMAGE_HANDLE;

    bool achieved = false;
    GetAchievement(pchName, &achieved);

    std::string ach_name(pchName);
    // here we force load in case the game has a lot of achievements, because otherwise some games might timeout
    // this somewhat defeats the purpose of background loading but a timeout is worse
    int handle = get_achievement_icon_handle(ach_name, achieved, true);
    if (Settings::UNLOADED_IMAGE_HANDLE == handle) { // if the background callback didn't get a chance to load this one yet
        handle = Settings::INVALID_IMAGE_HANDLE;
    }

    UserAchievementIconFetched_t data{};
    data.m_bAchieved = achieved ;
    data.m_nGameID = settings->get_local_game_id();
    data.m_nIconHandle = handle;
    ach_name.copy(data.m_rgchAchievementName, sizeof(data.m_rgchAchievementName));

    callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    return handle;
}

int Steam_User_Stats::get_achievement_icon_handle( const std::string &ach_name, bool achieved, bool force_load )
{
    PRINT_DEBUG("'%s', achieved=%i, force=%i", ach_name.c_str(), (int)achieved, (int)force_load);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(ach_name);
    } catch(...) { }
    if (defined_achievements.end() == it) return Settings::INVALID_IMAGE_HANDLE;

    int handle = Settings::INVALID_IMAGE_HANDLE;
    if (settings->paginated_achievements_icons < 0) { // disabled functionality
        handle = Settings::INVALID_IMAGE_HANDLE;
    } else if (settings->paginated_achievements_icons == 0) { // load the icon only when requested
        handle = load_ach_icon(*it, achieved);
    } else { // depend on the periodic callback to load the icon
        if (force_load) {
            handle = load_ach_icon(*it, achieved);
        } else {
            const char *icon_handle_key = achieved ? "icon_handle" : "icon_gray_handle";
            handle = it->value(icon_handle_key, Settings::UNLOADED_IMAGE_HANDLE);
        }
    }
    
    PRINT_DEBUG("returned handle = %i", handle);
    return handle;
}

std::string Steam_User_Stats::get_achievement_icon_name( const char *pchName, bool pbAchieved )
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (!pchName) return "";

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(pchName);
    } catch(...) { }
    if (defined_achievements.end() == it) return "";

    try {
        if (pbAchieved) return it.value()["icon"].get<std::string>();
        
        std::string locked_icon = it.value().value("icon_gray", std::string());
        if (locked_icon.size()) return locked_icon;
        else return it.value().value("icongray", std::string()); // old format
    } catch (...) {}

    return "";
}


// Get general attributes for an achievement. Accepts the following keys:
// - "name" and "desc" for retrieving the localized achievement name and description (returned in UTF8)
// - "hidden" for retrieving if an achievement is hidden (returns "0" when not hidden, "1" when hidden)
const char * Steam_User_Stats::GetAchievementDisplayAttribute( const char *pchName, const char *pchKey )
{
    PRINT_DEBUG("[%s] [%s]", pchName, pchKey);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (!pchName || !pchKey || !pchKey[0]) return "";

    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(pchName);
    } catch(...) { }
    if (defined_achievements.end() == it) return "";

    if (strncmp(pchKey, "name", sizeof("name")) == 0) {
        try {
            return it.value()["displayName"].get_ptr<std::string*>()->c_str();
        } catch (...) {}
    } else if (strncmp(pchKey, "desc", sizeof("desc")) == 0) {
        try {
            return it.value()["description"].get_ptr<std::string*>()->c_str();
        } catch (...) {}
    } else if (strncmp(pchKey, "hidden", sizeof("hidden")) == 0) {
        try {
            return it.value()["hidden"].get_ptr<std::string*>()->c_str();
        } catch (...) {}
    }

    return "";
}


// Achievement progress - triggers an AchievementProgress callback, that is all.
// Calling this w/ N out of N progress will NOT set the achievement, the game must still do that.
bool Steam_User_Stats::IndicateAchievementProgress( const char *pchName, uint32 nCurProgress, uint32 nMaxProgress )
{
    PRINT_DEBUG("'%s' %u %u", pchName, nCurProgress, nMaxProgress);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (!pchName) return false;
    if (nCurProgress >= nMaxProgress) return false;

    std::string ach_name(pchName);

    // find in achievements.json
    nlohmann::detail::iter_impl<nlohmann::json> it = defined_achievements.end();
    try {
        it = defined_achievements_find(ach_name);
    } catch(...) { }
    if (defined_achievements.end() == it) return false;

    // get actual name from achievements.json
    std::string actual_ach_name{};
    try {
        actual_ach_name = it->value("name", std::string());
    } catch (...) { }
    if (actual_ach_name.empty()) { // fallback
        actual_ach_name = ach_name;
    }

    // check if already achieved
    bool achieved = false;
    try {
        auto ach = user_achievements.find(actual_ach_name);
        if (ach != user_achievements.end()) {
            achieved = ach->value("earned", false);
        }
    } catch (...) { }
    if (achieved) return false;

    // save new progress
    try {
        auto old_progress = user_achievements.value(actual_ach_name, nlohmann::json{}).value("progress", ~nCurProgress);
        if (old_progress != nCurProgress) {
            user_achievements[actual_ach_name]["progress"] = nCurProgress;
            user_achievements[actual_ach_name]["max_progress"] = nMaxProgress;
            
            save_achievements();
            
            overlay->AddAchievementNotification(actual_ach_name, user_achievements[actual_ach_name], true);
        }
    } catch (...) {}

    {
        UserStatsStored_t data{};
        data.m_eResult = EResult::k_EResultOK;
        data.m_nGameID = settings->get_local_game_id().ToUint64();
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    }

    {
        UserAchievementStored_t data{};
        data.m_nGameID = settings->get_local_game_id().ToUint64();
        data.m_bGroupAchievement = false;
        data.m_nCurProgress = nCurProgress;
        data.m_nMaxProgress = nMaxProgress;
        ach_name.copy(data.m_rgchAchievementName, sizeof(data.m_rgchAchievementName) - 1);

        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    }

    return true;
}


// Used for iterating achievements. In general games should not need these functions because they should have a
// list of existing achievements compiled into them
uint32 Steam_User_Stats::GetNumAchievements()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    return (uint32)defined_achievements.size();
}

// Get achievement name iAchievement in [0,GetNumAchievements)
const char * Steam_User_Stats::GetAchievementName( uint32 iAchievement )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (iAchievement >= sorted_achievement_names.size()) {
        return "";
    }

    return sorted_achievement_names[iAchievement].c_str();
}


// Friends achievements

bool Steam_User_Stats::GetUserAchievement( CSteamID steamIDUser, const char *pchName, bool *pbAchieved )
{
    PRINT_DEBUG("%s", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    if (!pchName) return false;

    if (steamIDUser == settings->get_local_steam_id()) {
        return GetAchievement(pchName, pbAchieved);
    }

    return false;
}

// See notes for GetAchievementAndUnlockTime above
bool Steam_User_Stats::GetUserAchievementAndUnlockTime( CSteamID steamIDUser, const char *pchName, bool *pbAchieved, uint32 *punUnlockTime )
{
    PRINT_DEBUG("%s", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (!pchName) return false;

    if (steamIDUser == settings->get_local_steam_id()) {
        return GetAchievementAndUnlockTime(pchName, pbAchieved, punUnlockTime);
    }
    return false;
}


static size_t global_ach_percent_curl_write(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    static_cast<std::string *>(userp)->append(static_cast<char *>(contents), real_size);
    return real_size;
}

// Requests that Steam fetch data on the percentage of players who have received each achievement
// for the game globally.
// This call is asynchronous, with the result returned in GlobalAchievementPercentagesReady_t.
STEAM_CALL_RESULT( GlobalAchievementPercentagesReady_t )
SteamAPICall_t Steam_User_Stats::RequestGlobalAchievementPercentages()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    int64_t cache_ttl = (int64_t)settings->achievements_cache_ttl;
    uint64 game_id = settings->get_local_game_id().ToUint64();

    // --- helper: commit parsed percentages under the caller's lock ---
    auto commit_percentages = [this](std::map<std::string, float> &&percentages) {
        global_achievement_percentages = std::move(percentages);
        sorted_global_achievement_percentages.clear();
        for (const auto &kv : global_achievement_percentages) {
            sorted_global_achievement_percentages.emplace_back(kv.first, kv.second);
        }
        std::sort(sorted_global_achievement_percentages.begin(), sorted_global_achievement_percentages.end(),
            [](const std::pair<std::string, float> &a, const std::pair<std::string, float> &b) {
                return a.second > b.second;
            });
        global_achievement_percentages_populated = true;
        update_user_achievements_with_global_percent();
    };

    // --- try disk cache first (only if not already populated) ---
    bool cache_fresh = false;
    if (!global_achievement_percentages_populated) {
        nlohmann::json cache{};
        if (local_storage->load_json_file("", steam_ach_percentages_cache_file, cache)) {
            try {
                int64_t ts = cache.value("fetched_at", (int64_t)0);
                int64_t now = (int64_t)std::time(nullptr);
                if ((now - ts) < cache_ttl && cache.contains("response")) {
                    // parse using same logic as the live fetch
                    std::map<std::string, float> percentages{};
                    for (const auto &entry : cache["response"].at("achievementpercentages").at("achievements")) {
                        auto &pct_val = entry.at("percent");
                        float pct = pct_val.is_string() ? std::stof(pct_val.get<std::string>()) : pct_val.get<float>();
                        percentages[entry.at("name").get<std::string>()] = pct;
                    }
                    cache_fresh = true;
                    if (!percentages.empty()) {
                        commit_percentages(std::move(percentages));
                        PRINT_DEBUG("Steam global %%: loaded %zu entries from cache", global_achievement_percentages.size());
                    } else {
                        PRINT_DEBUG("Steam global %%: cache hit but empty (API returned no data last time)");
                    }
                } else {
                    PRINT_DEBUG("Steam global %%: cache expired or invalid, will re-fetch");
                }
            } catch (...) {}
        }
    }

    // --- fire immediate callback if we have data (from cache or prior fetch) ---
    if (global_achievement_percentages_populated) {
        GlobalAchievementPercentagesReady_t data{};
        data.m_eResult = EResult::k_EResultOK;
        data.m_nGameID = game_id;
        auto ret = callback_results->addCallResult(data.k_iCallback, &data, sizeof(data));
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));

        // If cache was stale kick off background refetch (result updates data silently, no second callback)
        if (!cache_fresh && !global_achievement_percentages_fetching
                && !settings->disable_networking && !settings->is_offline()) {
            global_achievement_percentages_fetching = true;
            std::thread([this, game_id, cache_ttl]() {
                std::string url = "https://api.steampowered.com/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v2/?gameid=" + std::to_string(game_id);
                std::string response{};
                CURL *curl = curl_easy_init();
                if (curl) {
                    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, global_ach_percent_curl_write);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
                    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.142.86 Safari/537.36");
                    curl_easy_perform(curl);
                    curl_easy_cleanup(curl);
                }
                std::map<std::string, float> percentages{};
                if (!response.empty()) {
                    try {
                        auto j = nlohmann::json::parse(response);
                        for (const auto &entry : j.at("achievementpercentages").at("achievements")) {
                            auto &pct_val = entry.at("percent");
                            float pct = pct_val.is_string() ? std::stof(pct_val.get<std::string>()) : pct_val.get<float>();
                            percentages[entry.at("name").get<std::string>()] = pct;
                        }
                    } catch (...) {}
                }
                // always save raw API response as cache
                {
                    nlohmann::json to_save;
                    to_save["fetched_at"] = (int64_t)std::time(nullptr);
                    to_save["appid"] = game_id;
                    try { to_save["response"] = nlohmann::json::parse(response); } catch (...) { to_save["response"] = nullptr; }
                    local_storage->write_json_file("", steam_ach_percentages_cache_file, to_save);
                }
                {
                    std::lock_guard<std::recursive_mutex> lock(global_mutex);
                    global_achievement_percentages_fetching = false;
                    if (!percentages.empty()) {
                        global_achievement_percentages = std::move(percentages);
                        sorted_global_achievement_percentages.clear();
                        for (const auto &kv : global_achievement_percentages)
                            sorted_global_achievement_percentages.emplace_back(kv.first, kv.second);
                        std::sort(sorted_global_achievement_percentages.begin(), sorted_global_achievement_percentages.end(),
                            [](const std::pair<std::string, float> &a, const std::pair<std::string, float> &b) { return a.second > b.second; });
                        global_achievement_percentages_populated = true;
                        update_user_achievements_with_global_percent();
                        PRINT_DEBUG("Steam global %%: background refetch done, %zu entries", global_achievement_percentages.size());
                    } else {
                        PRINT_DEBUG("Steam global %%: background refetch returned no data");
                    }
                }
            }).detach();
        }
        return ret;
    }

    // --- no data yet: offline / networking disabled → immediate empty callback ---
    bool can_fetch = !settings->disable_networking && !settings->is_offline()
        && !global_achievement_percentages_fetching;
    if (!can_fetch) {
        GlobalAchievementPercentagesReady_t data{};
        data.m_eResult = EResult::k_EResultOK;
        data.m_nGameID = game_id;
        auto ret = callback_results->addCallResult(data.k_iCallback, &data, sizeof(data));
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
        return ret;
    }

    // --- start background fetch, callback fires on completion ---
    global_achievement_percentages_fetching = true;
    auto call_res_id = callback_results->reserveCallResult();
    std::string url = "https://api.steampowered.com/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v2/?gameid=" + std::to_string(game_id);

    std::thread([this, call_res_id, url, game_id, cache_ttl]() {
        std::string response{};
        CURL *curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, global_ach_percent_curl_write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.142.86 Safari/537.36");
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }

        std::map<std::string, float> percentages{};
        if (!response.empty()) {
            try {
                auto j = nlohmann::json::parse(response);
                for (const auto &entry : j.at("achievementpercentages").at("achievements")) {
                    auto &pct_val = entry.at("percent");
                    float pct = pct_val.is_string() ? std::stof(pct_val.get<std::string>()) : pct_val.get<float>();
                    percentages[entry.at("name").get<std::string>()] = pct;
                }
            } catch (...) {}
        }

        // always save raw API response as cache (even on failure, to honor TTL and avoid hammering)
        {
            nlohmann::json to_save;
            to_save["fetched_at"] = (int64_t)std::time(nullptr);
            to_save["appid"] = game_id;
            try { to_save["response"] = nlohmann::json::parse(response); } catch (...) { to_save["response"] = nullptr; }
            local_storage->write_json_file("", steam_ach_percentages_cache_file, to_save);
        }

        {
            std::lock_guard<std::recursive_mutex> lock(global_mutex);
            global_achievement_percentages_fetching = false;
            if (!percentages.empty()) {
                global_achievement_percentages = std::move(percentages);
                sorted_global_achievement_percentages.clear();
                for (const auto &kv : global_achievement_percentages)
                    sorted_global_achievement_percentages.emplace_back(kv.first, kv.second);
                std::sort(sorted_global_achievement_percentages.begin(), sorted_global_achievement_percentages.end(),
                    [](const std::pair<std::string, float> &a, const std::pair<std::string, float> &b) { return a.second > b.second; });
                global_achievement_percentages_populated = true;
                update_user_achievements_with_global_percent();
                PRINT_DEBUG("Steam global %%: fetched %zu entries", global_achievement_percentages.size());
            } else {
                PRINT_DEBUG("Steam global %%: API returned no data (response: %s)", response.substr(0, 200).c_str());
            }
        }

        GlobalAchievementPercentagesReady_t data{};
        data.m_eResult = !percentages.empty() ? EResult::k_EResultOK : EResult::k_EResultFail;
        data.m_nGameID = game_id;
        callback_results->addCallResult(call_res_id, data.k_iCallback, &data, sizeof(data));
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    }).detach();

    return call_res_id;
}


// Write "global_percent" into each entry of user_achievements and persist to disk.
// Must be called under global_mutex.
void Steam_User_Stats::update_user_achievements_with_global_percent()
{
    bool changed = false;
    for (auto &[name, pct] : global_achievement_percentages) {
        auto it = user_achievements.find(name);
        if (it != user_achievements.end()) {
            float rounded = std::round(pct * 10.0f) / 10.0f;
            float old_pct = it->value("global_percent", -1.0f);
            if (old_pct != rounded) {
                (*it)["global_percent"] = rounded;
                changed = true;
            }
        }
    }
    if (changed) save_achievements();
}


// Async fetch of SteamHunters achievement groups and all per-achievement metadata.
// Results are cached on disk as achievements_sh.json (next to achievements.json).
// Cache TTL is governed by settings->achievements_cache_ttl (default 86400 s).
void Steam_User_Stats::RequestSteamHuntersData()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (steamhunters_data_fetching || steamhunters_data_populated) return;
    steamhunters_data_fetching = true;

    uint64 app_id = settings->get_local_game_id().AppID();
    int64_t cache_ttl = (int64_t)settings->achievements_cache_ttl;

    std::thread([this, app_id, cache_ttl]() {

        // --- helper: simple blocking GET via curl ---
        auto curl_get = [](const std::string &url) -> std::string {
            std::string response{};
            CURL *curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, global_ach_percent_curl_write);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.142.86 Safari/537.36");
                curl_easy_perform(curl);
                curl_easy_cleanup(curl);
            }
            return response;
        };

        // --- helper: parse groups array from JSON ---
        auto parse_groups = [](const nlohmann::json &j) -> std::vector<SteamHunters_AchievementGroup> {
            std::vector<SteamHunters_AchievementGroup> groups{};
            if (!j.contains("groups")) return groups;
            for (const auto &g : j.at("groups")) {
                SteamHunters_AchievementGroup grp{};
                if (g.contains("name")       && g["name"].is_string())         grp.name       = g["name"].get<std::string>();
                if (g.contains("dlcAppId")   && g["dlcAppId"].is_number())     grp.dlcAppId   = g["dlcAppId"].get<int>();
                if (g.contains("dlcAppName") && g["dlcAppName"].is_string())   grp.dlcAppName = g["dlcAppName"].get<std::string>();
                if (g.contains("achievementApiNames"))
                    for (const auto &n : g["achievementApiNames"])
                        if (n.is_string()) grp.achievementApiNames.push_back(n.get<std::string>());
                groups.push_back(std::move(grp));
            }
            return groups;
        };

        // --- helper: parse achievements array from JSON ---
        auto parse_achievements = [](const nlohmann::json &j) -> std::map<std::string, SteamHunters_AchievementData> {
            std::map<std::string, SteamHunters_AchievementData> data{};
            if (!j.is_array()) return data;
            for (const auto &entry : j) {
                if (!entry.contains("apiName")) continue;
                std::string api_name = entry["apiName"].get<std::string>();
                SteamHunters_AchievementData d{};
                d.steamPercentage = entry.value("steamPercentage", -1.0f);
                d.localPercentage = entry.value("localPercentage", -1.0f);
                d.points          = entry.value("points",          0);
                d.steamPoints     = entry.value("steamPoints",     0);
                d.obtainability   = entry.value("obtainability",   0);
                data[api_name] = d;
            }
            return data;
        };

        std::vector<SteamHunters_AchievementGroup>          groups{};
        std::map<std::string, SteamHunters_AchievementData> ach_data{};
        bool loaded_from_cache = false;

        // ---- try loading from disk cache first ----
        {
            nlohmann::json cache{};
            if (local_storage->load_json_file("", steamhunters_cache_file, cache)) {
                try {
                    int64_t ts = cache.value("fetched_at", (int64_t)0);
                    int64_t now = (int64_t)std::time(nullptr);
                    if ((now - ts) < cache_ttl) {
                        if (cache.contains("groups"))       groups    = parse_groups(cache["groups"]);
                        if (cache.contains("achievements")) ach_data  = parse_achievements(cache["achievements"]);
                        loaded_from_cache = true;
                        PRINT_DEBUG("SteamHunters: loaded from cache (%zu groups, %zu achs)", groups.size(), ach_data.size());
                    } else {
                        PRINT_DEBUG("SteamHunters: cache expired (ttl=%llds), re-fetching", cache_ttl);
                    }
                } catch (...) {}
            }
        }

        // ---- fetch from network if cache miss or expired ----
        if (!loaded_from_cache && !settings->disable_networking && !settings->is_offline()) {
            nlohmann::json groups_raw{};
            nlohmann::json achs_raw = nlohmann::json::array();

            // 1. Groups
            {
                std::string url = "https://steamhunters.com/api/GetAchievementGroups/v1?appid=" + std::to_string(app_id);
                std::string body = curl_get(url);
                if (!body.empty()) {
                    try {
                        groups_raw = nlohmann::json::parse(body);
                        groups = parse_groups(groups_raw);
                        PRINT_DEBUG("SteamHunters: fetched %zu groups for app %llu", groups.size(), app_id);
                    } catch (const std::exception &e) {
                        (void)e;
                        PRINT_DEBUG("SteamHunters: groups parse error: %s", e.what());
                    }
                }
            }

            // 2. Per-achievement data
            {
                std::string url = "https://steamhunters.com/api/apps/" + std::to_string(app_id) + "/achievements";
                std::string body = curl_get(url);
                if (!body.empty()) {
                    try {
                        achs_raw = nlohmann::json::parse(body);
                        ach_data = parse_achievements(achs_raw);
                        PRINT_DEBUG("SteamHunters: fetched %zu achievements for app %llu", ach_data.size(), app_id);
                    } catch (const std::exception &e) {
                        (void)e;
                        PRINT_DEBUG("SteamHunters: achievements parse error: %s", e.what());
                    }
                }
            }

            // ---- save to disk cache ----
            if (!groups_raw.is_null() || !achs_raw.empty()) {
                nlohmann::json cache{};
                cache["fetched_at"]   = (int64_t)std::time(nullptr);
                cache["appid"]        = app_id;
                cache["groups"]       = groups_raw.is_null() ? nlohmann::json::object() : groups_raw;
                cache["achievements"] = achs_raw;
                local_storage->write_json_file("", steamhunters_cache_file, cache);
                PRINT_DEBUG("SteamHunters: cache saved");
            }
        }

        // ---- commit results under lock ----
        {
            std::lock_guard<std::recursive_mutex> lock(global_mutex);
            steamhunters_data_fetching = false;
            steamhunters_data_populated = true;
            if (!groups.empty())    steamhunters_achievement_groups  = std::move(groups);
            if (!ach_data.empty())  steamhunters_achievement_data    = std::move(ach_data);

            // use SteamHunters steamPercentage as fallback if Steam API hasn't returned yet
            if (!global_achievement_percentages_populated && !steamhunters_achievement_data.empty()) {
                for (const auto &[name, d] : steamhunters_achievement_data) {
                    if (d.steamPercentage >= 0.0f)
                        global_achievement_percentages[name] = d.steamPercentage;
                }
                sorted_global_achievement_percentages.clear();
                for (const auto &kv : global_achievement_percentages) {
                    sorted_global_achievement_percentages.emplace_back(kv.first, kv.second);
                }
                std::sort(sorted_global_achievement_percentages.begin(), sorted_global_achievement_percentages.end(),
                    [](const std::pair<std::string, float> &a, const std::pair<std::string, float> &b) {
                        return a.second > b.second;
                    });
                if (!global_achievement_percentages.empty()) {
                    global_achievement_percentages_populated = true;
                    update_user_achievements_with_global_percent();
                    PRINT_DEBUG("SteamHunters: using SH percentages as fallback for global %%");
                }
            }
        }

        if (overlay) overlay->UpdateSteamHuntersData();
    }).detach();
}


// Async fetch + disk cache of the SteamCardExchange game page HTML.
// Cache file: steamcardexchange_sce.html (raw HTML bytes), TTL same as other caches.
// On cache hit the in-memory sce_html is populated without a network request.
void Steam_User_Stats::RequestSteamCardExchangeData()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (sce_data_fetching || sce_data_populated) return;
    sce_data_fetching = true;

    uint64 app_id = settings->get_local_game_id().AppID();
    int64_t cache_ttl = (int64_t)settings->achievements_cache_ttl;

    std::thread([this, app_id, cache_ttl]() {

        auto curl_get = [](const std::string &url) -> std::string {
            std::string response{};
            CURL *curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, global_ach_percent_curl_write);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.142.86 Safari/537.36");
                curl_easy_perform(curl);
                curl_easy_cleanup(curl);
            }
            return response;
        };

        // ---- helpers for JSON serialization of SceGameData ----
        // SCE page order: each entry is (json_key, SceItemType)
        const std::pair<const char*, SceItemType> sce_type_order[] = {
            {"cards",                     SceItemType::TradingCard},
            {"foil_cards",                SceItemType::FoilCard},
            {"booster_packs",             SceItemType::BoosterPack},
            {"badges",                    SceItemType::Badge},
            {"foil_badges",               SceItemType::FoilBadge},
            {"emoticons",                 SceItemType::Emoticon},
            {"backgrounds",               SceItemType::Background},
            {"animated_backgrounds",      SceItemType::AnimatedBackground},
            {"animated_mini_backgrounds", SceItemType::AnimatedMiniBackground},
            {"profiles",                  SceItemType::Profile},
            {"avatar_frames",             SceItemType::AvatarFrame},
            {"animated_avatars",          SceItemType::AnimatedAvatar},
        };

        auto serialize_game_data = [&](const SceGameData &gd) -> nlohmann::json {
            nlohmann::json root = nlohmann::json::object();
            root["appid"] = gd.appid;
            nlohmann::json series_arr = nlohmann::json::array();
            for (const auto &s : gd.series) {
                nlohmann::json sobj = nlohmann::json::object();
                sobj["series_number"] = s.series_number;
                if (!s.series_name.empty()) sobj["series_name"] = s.series_name;
                for (const auto &[key, itype] : sce_type_order) {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto &item : s.items) {
                        if (item.type != itype) continue;
                        nlohmann::json iobj = nlohmann::json::object();
                        iobj["name"] = item.name;
                        if (item.slot > 0)                  iobj["slot"]             = item.slot;
                        if (item.total > 0)                 iobj["total"]            = item.total;
                        if (!item.icon_url.empty())         iobj["icon_url"]         = item.icon_url;
                        if (!item.wallpaper_url.empty())    iobj["wallpaper_url"]    = item.wallpaper_url;
                        if (!item.animated_url.empty())     iobj["animated_url"]     = item.animated_url;
                        if (!item.market_hash_name.empty()) iobj["market_hash_name"] = item.market_hash_name;
                        if (!item.price_text.empty())       iobj["price_text"]       = item.price_text;
                        if (item.badge_level > 0)           iobj["badge_level"]      = item.badge_level;
                        if (item.badge_xp > 0)             iobj["badge_xp"]         = item.badge_xp;
                        if (!item.emoticon_name.empty())    iobj["emoticon_name"]    = item.emoticon_name;
                        if (!item.rarity.empty())           iobj["rarity"]           = item.rarity;
                        if (!item.video_webm_url.empty())   iobj["video_webm_url"]   = item.video_webm_url;
                        if (!item.video_mp4_url.empty())    iobj["video_mp4_url"]    = item.video_mp4_url;
                        if (!item.static_img_url.empty())   iobj["static_img_url"]   = item.static_img_url;
                        if (!item.points_price.empty())     iobj["points_price"]     = item.points_price;
                        if (!item.preview_url.empty())      iobj["preview_url"]      = item.preview_url;
                        arr.push_back(std::move(iobj));
                    }
                    if (!arr.empty()) sobj[key] = std::move(arr);
                }
                series_arr.push_back(std::move(sobj));
            }
            root["series"] = std::move(series_arr);
            return root;
        };

        auto deserialize_game_data = [&](const nlohmann::json &root) -> SceGameData {
            SceGameData gd{};
            gd.appid = root.value("appid", (uint32)0);
            for (const auto &sobj : root.value("series", nlohmann::json::array())) {
                SceSeries s{};
                s.series_number = sobj.value("series_number", 0);
                s.series_name   = sobj.value("series_name",   std::string{});
                for (const auto &[key, itype] : sce_type_order) {
                    if (!sobj.contains(key)) continue;
                    for (const auto &iobj : sobj[key]) {
                        SceItem item{};
                        item.type             = itype;
                        item.series           = s.series_number;
                        item.name             = iobj.value("name",             std::string{});
                        item.slot             = iobj.value("slot",             0);
                        item.total            = iobj.value("total",            0);
                        item.icon_url         = iobj.value("icon_url",         std::string{});
                        item.wallpaper_url    = iobj.value("wallpaper_url",    std::string{});
                        item.animated_url     = iobj.value("animated_url",     std::string{});
                        item.market_hash_name = iobj.value("market_hash_name", std::string{});
                        item.price_text       = iobj.value("price_text",       std::string{});
                        item.badge_level      = iobj.value("badge_level",      0);
                        item.badge_xp         = iobj.value("badge_xp",         0);
                        item.emoticon_name    = iobj.value("emoticon_name",    std::string{});
                        item.rarity           = iobj.value("rarity",           std::string{});
                        item.video_webm_url   = iobj.value("video_webm_url",   std::string{});
                        item.video_mp4_url    = iobj.value("video_mp4_url",    std::string{});
                        item.static_img_url   = iobj.value("static_img_url",   std::string{});
                        item.points_price     = iobj.value("points_price",     std::string{});
                        item.preview_url      = iobj.value("preview_url",      std::string{});
                        s.items.push_back(std::move(item));
                    }
                }
                gd.series.push_back(std::move(s));
            }
            return gd;
        };

        std::string html{};
        SceGameData parsed{};
        bool loaded_from_cache = false;
        nlohmann::json stale_jcache{};   // expired JSON kept for size-based revalidation
        bool have_stale_json = false;

        // ---- try loading parsed JSON cache first ----
        {
            nlohmann::json jcache{};
            if (local_storage->load_json_file("", steamcardexchange_json_cache_file, jcache)) {
                try {
                    int64_t ts  = jcache.value("fetched_at", (int64_t)0);
                    int64_t now = (int64_t)std::time(nullptr);
                    if ((now - ts) < cache_ttl && jcache.contains("series")) {
                        parsed = deserialize_game_data(jcache);
                        loaded_from_cache = true;
                        PRINT_DEBUG("SteamCardExchange: loaded parsed data from JSON cache (%zu series)", parsed.series.size());
                    } else {
                        // keep stale data: if new HTML is the same size we can revalidate without re-parsing
                        if (jcache.contains("series")) {
                            stale_jcache   = std::move(jcache);
                            have_stale_json = true;
                        }
                        PRINT_DEBUG("SteamCardExchange: JSON cache expired (ttl=%llds), re-fetching", cache_ttl);
                    }
                } catch (...) {}
            }
        }

        // ---- fall back to raw HTML cache ----
        if (!loaded_from_cache) {
            uint64_t file_ts = local_storage->file_timestamp("", steamcardexchange_cache_file);
            int64_t now = (int64_t)std::time(nullptr);
            if (file_ts > 0 && (now - (int64_t)file_ts) < cache_ttl) {
                unsigned int sz = local_storage->file_size("", steamcardexchange_cache_file);
                if (sz > 0) {
                    std::string buf(sz, '\0');
                    int got = local_storage->get_data("", steamcardexchange_cache_file, &buf[0], sz);
                    if (got > 0) {
                        html = buf.substr(0, (size_t)got);
                        PRINT_DEBUG("SteamCardExchange: loaded HTML from cache, will re-parse (%u bytes)", sz);
                    }
                }
            } else if (file_ts > 0) {
                PRINT_DEBUG("SteamCardExchange: HTML cache expired (ttl=%llds), re-fetching", cache_ttl);
            }
        }

        // ---- fetch from network if both caches miss ----
        if (!loaded_from_cache && html.empty() && !settings->disable_networking && !settings->is_offline()) {
            // record old HTML size before fetching so we can skip re-parsing if content is unchanged
            unsigned int old_html_sz = local_storage->file_size("", steamcardexchange_cache_file);

            std::string url = "https://www.steamcardexchange.net/index.php?gamepage-appid-" + std::to_string(app_id);
            html = curl_get(url);
            if (!html.empty()) {
                // size match + stale JSON present → revalidate without re-parsing
                if (have_stale_json && old_html_sz > 0 && html.size() == (size_t)old_html_sz) {
                    PRINT_DEBUG("SteamCardExchange: new HTML same size (%u bytes), revalidating JSON cache", old_html_sz);
                    try {
                        stale_jcache["fetched_at"] = (int64_t)std::time(nullptr);
                        local_storage->write_json_file("", steamcardexchange_json_cache_file, stale_jcache);
                        parsed = deserialize_game_data(stale_jcache);
                        loaded_from_cache = true;
                    } catch (...) {}
                }
                local_storage->store_data("", steamcardexchange_cache_file, &html[0], (unsigned int)html.size());
                if (loaded_from_cache) {
                    html.clear(); // no need to keep full HTML in memory
                } else {
                    PRINT_DEBUG("SteamCardExchange: fetched and cached HTML (%zu bytes) for app %llu", html.size(), app_id);
                }
            } else {
                PRINT_DEBUG("SteamCardExchange: fetch returned empty body for app %llu", app_id);
            }
        }

        // ---- parse HTML if we didn't load from JSON cache ----
        if (!loaded_from_cache && !html.empty()) {
            parsed = ParseSteamCardExchangeHtml(html, (uint32)app_id);
            PRINT_DEBUG("SteamCardExchange: parsed %zu series from HTML", parsed.series.size());

            // persist parsed data as JSON so future startups skip HTML parsing
            if (!parsed.series.empty()) {
                try {
                    nlohmann::json jout = serialize_game_data(parsed);
                    jout["fetched_at"] = (int64_t)std::time(nullptr);
                    local_storage->write_json_file("", steamcardexchange_json_cache_file, jout);
                    PRINT_DEBUG("SteamCardExchange: JSON cache written");
                } catch (...) {}
            }
        }

        // ---- commit results under lock ----
        {
            std::lock_guard<std::recursive_mutex> lock(global_mutex);
            sce_data_fetching  = false;
            sce_data_populated = true;
            sce_html           = std::move(html);
            sce_game_data      = std::move(parsed);
        }
    }).detach();
}


// Collect all unique non-empty URLs from all items in sce_game_data, download any that
// are not yet cached to disk (sce_assets/ subfolder), then notify the overlay.
// A 200ms delay is inserted between each network request to avoid hammering the Steam CDN.
void Steam_User_Stats::RequestSceAssetDownload()
{
    PRINT_DEBUG_ENTRY();

    // Guard: only one download run at a time; also refuse if catalog not ready
    {
        std::lock_guard<std::recursive_mutex> lock(global_mutex);
        if (sce_assets_downloading || !sce_data_populated) return;
        if (sce_game_data.series.empty()) return;
        sce_assets_downloading = true;
        sce_assets_downloaded  = 0;
        sce_assets_skipped     = 0;
        sce_assets_total       = 0;
        for (int i = 0; i < SCE_NUM_TYPES; ++i) {
            sce_type_progress[i].total      = 0;
            sce_type_progress[i].downloaded = 0;
            sce_type_progress[i].skipped    = 0;
            sce_type_progress[i].current    = 0;
        }
    }

    std::thread([this]() {

        // Maps SceItemType → human-readable subfolder name matching sce_type_order keys
        auto type_subfolder = [](SceItemType t) -> const char * {
            switch (t) {
                case SceItemType::TradingCard:            return "cards";
                case SceItemType::FoilCard:               return "foil_cards";
                case SceItemType::BoosterPack:            return "booster_packs";
                case SceItemType::Badge:                  return "badges";
                case SceItemType::FoilBadge:              return "foil_badges";
                case SceItemType::Emoticon:               return "emoticons";
                case SceItemType::Background:             return "backgrounds";
                case SceItemType::AnimatedBackground:     return "animated_backgrounds";
                case SceItemType::AnimatedMiniBackground: return "animated_mini_backgrounds";
                case SceItemType::Profile:                return "profiles";
                case SceItemType::AvatarFrame:            return "avatar_frames";
                case SceItemType::AnimatedAvatar:         return "animated_avatars";
                case SceItemType::AnimatedSticker:        return "animated_stickers";
                case SceItemType::StartupMovie:           return "startup_movies";
                default:                                  return "misc";
            }
        };

        // Sanitize a string for use as a directory/file name component
        auto sanitize = [](std::string s) -> std::string {
            for (char &c : s) {
                if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                    c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                    c = '_';
            }
            return s;
        };

        // Extract just the filename portion of a URL (last path segment, no query)
        auto url_filename = [](const std::string &url) -> std::string {
            size_t slash = url.rfind('/');
            std::string name = (slash != std::string::npos) ? url.substr(slash + 1) : url;
            size_t q = name.find('?');
            if (q != std::string::npos) name.resize(q);
            if (name.find('.') == std::string::npos) name += ".png";
            return name;
        };

        struct AssetEntry {
            std::string url;
            std::string folder;   // relative folder inside sce_assets_folder
            std::string filename; // e.g. "01_wallpaper_abc123.jpg"
            int         type_idx; // (int)SceItemType — for per-type progress
        };

        // ---- collect unique URLs, organized by series / type ----
        // Also tally per-type totals (each unique URL = 1 work unit)
        std::map<int, uint32_t> type_totals; // type_idx -> count
        std::vector<AssetEntry> queue;
        {
            std::set<std::string> seen_urls;
            std::lock_guard<std::recursive_mutex> lock(global_mutex);

            for (const auto &s : sce_game_data.series) {
                // "Series 01 - Name" or just "Series 01"
                char ser_num_str[8]{};
                snprintf(ser_num_str, sizeof(ser_num_str), "%02d", s.series_number);
                std::string ser_dir = std::string("Series ") + ser_num_str;
                if (!s.series_name.empty()) ser_dir += " - " + sanitize(s.series_name);

                // count items per type for slot numbering
                std::map<SceItemType, int> slot_counter;

                for (const auto &item : s.items) {
                    int slot_idx = ++slot_counter[item.type];
                    char prefix[16]{};
                    snprintf(prefix, sizeof(prefix), "%02d_", slot_idx);

                    std::string type_dir = ser_dir + PATH_SEPARATOR + type_subfolder(item.type);
                    std::string full_folder = std::string(sce_assets_folder) + PATH_SEPARATOR + type_dir;

                    // which URLs does this item carry?
                    struct UrlRole { const std::string *url; const char *role; };
                    UrlRole roles[] = {
                        { &item.icon_url,       "icon"       },
                        { &item.wallpaper_url,  "wallpaper"  },
                        { &item.animated_url,   "animated"   },
                        { &item.static_img_url, "static"     },
                        { &item.video_mp4_url,  "video_mp4"  },
                        { &item.video_webm_url, "video_webm" },
                    };
                    for (const auto &r : roles) {
                        if (r.url->empty()) continue;
                        if (!seen_urls.insert(*r.url).second) continue; // duplicate

                        std::string orig_name = url_filename(*r.url);
                        // derive extension from original filename
                        std::string ext;
                        size_t dot = orig_name.rfind('.');
                        if (dot != std::string::npos) ext = orig_name.substr(dot); // e.g. ".jpg"

                        // filename: "01_icon_Name.jpg" (name sanitized, truncated to 48 chars)
                        std::string item_label = sanitize(item.name);
                        if (item_label.size() > 48) item_label.resize(48);
                        std::string filename = std::string(prefix) + r.role + "_" + item_label + ext;

                        int tidx = (int)item.type;
                        queue.push_back({ *r.url, full_folder, filename, tidx });
                        ++type_totals[tidx];
                    }

                    // For backgrounds: also download a CDN-resized thumbnail (300×180)
                    // saved as "01_thumb_wallpaper_Name.ext" for fast display in the browser.
                    bool is_bg_type = (item.type == SceItemType::Background ||
                                       item.type == SceItemType::AnimatedBackground ||
                                       item.type == SceItemType::AnimatedMiniBackground);
                    if (is_bg_type && !item.wallpaper_url.empty()) {
                        std::string thumb_url = item.wallpaper_url + "?size=300x180f";
                        if (seen_urls.insert(thumb_url).second) {
                            std::string orig_name = url_filename(item.wallpaper_url);
                            std::string ext;
                            size_t dot = orig_name.rfind('.');
                            if (dot != std::string::npos) ext = orig_name.substr(dot);
                            std::string item_label = sanitize(item.name);
                            if (item_label.size() > 48) item_label.resize(48);
                            std::string tfilename = std::string(prefix) + "thumb_wallpaper_" + item_label + ext;
                            int tidx = (int)item.type;
                            queue.push_back({ thumb_url, full_folder, tfilename, tidx });
                            ++type_totals[tidx];
                        }
                    }
                }
            }
        }

        sce_assets_total = (uint32_t)queue.size();
        for (const auto &[tidx, cnt] : type_totals)
            if (tidx >= 0 && tidx < SCE_NUM_TYPES) sce_type_progress[tidx].total = cnt;
        PRINT_DEBUG("SceAssets: %zu unique URLs to process", queue.size());

        // re-use the same curl setup as other fetches
        auto curl_get_binary = [](const std::string &url) -> std::string {
            std::string response{};
            CURL *curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, global_ach_percent_curl_write);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.142.86 Safari/537.36");
                curl_easy_perform(curl);
                curl_easy_cleanup(curl);
            }
            return response;
        };

        // Build up to 2 alternative CDN URLs for a given asset URL.
        // Steam serves /steamcommunity/public/images/items/ via three interchangeable CDN hosts:
        //   cdn.cloudflare.steamstatic.com  (Cloudflare — primary in SCE pages)
        //   steamcdn-a.akamaihd.net         (Akamai     — used by SCE for badges, Valve docs)
        //   cdn.fastly.steamstatic.com      (Fastly     — used by Steam store for achievement icons)
        // For any other host (e.g. steamcommunity.com/economy/profilebackground/...) no alternatives.
        auto build_cdn_alts = [](const std::string &url) -> std::vector<std::string> {
            static constexpr const char *kCdnHosts[] = {
                "cdn.cloudflare.steamstatic.com",
                "steamcdn-a.akamaihd.net",
                "cdn.fastly.steamstatic.com",
            };
            constexpr size_t kSchemeLen = 8; // strlen("https://")
            if (url.size() <= kSchemeLen) return {};
            size_t path_start = url.find('/', kSchemeLen);
            if (path_start == std::string::npos) return {};
            std::string host = url.substr(kSchemeLen, path_start - kSchemeLen);
            std::string path = url.substr(path_start);
            std::vector<std::string> alts;
            for (const char *h : kCdnHosts) {
                if (host == h) continue;
                alts.push_back(std::string("https://") + h + path);
                if (alts.size() >= 2) break;
            }
            return alts;
        };

        uint32_t downloaded = 0;
        uint32_t skipped    = 0;

        for (const auto &entry : queue) {
            int tidx = (entry.type_idx >= 0 && entry.type_idx < SCE_NUM_TYPES) ? entry.type_idx : -1;

            // skip if file already exists on disk
            if (local_storage->file_size(entry.folder, entry.filename) > 0) {
                ++skipped;
                sce_assets_skipped = skipped;
                if (tidx >= 0) { ++sce_type_progress[tidx].skipped; ++sce_type_progress[tidx].current; }
                PRINT_DEBUG("SceAssets: skip (exists) %s/%s", entry.folder.c_str(), entry.filename.c_str());
                continue;
            }

            if (tidx >= 0) {
                uint32_t cur = sce_type_progress[tidx].current.load() + 1;
                uint32_t tot = sce_type_progress[tidx].total.load();
                PRINT_DEBUG("SceAssets [%s %u/%u]: fetch %s",
                    SCE_TYPE_LABELS[tidx], cur, tot, entry.url.c_str());
            } else {
                PRINT_DEBUG("SceAssets: fetch %s -> %s/%s", entry.url.c_str(), entry.folder.c_str(), entry.filename.c_str());
            }

            std::string data = curl_get_binary(entry.url);

            // On failure, try up to 2 alternative CDN hosts before giving up
            if (data.empty()) {
                for (const auto &alt_url : build_cdn_alts(entry.url)) {
                    PRINT_DEBUG("SceAssets: primary failed, trying CDN alt: %s", alt_url.c_str());
                    data = curl_get_binary(alt_url);
                    if (!data.empty()) {
                        PRINT_DEBUG("SceAssets: CDN alt succeeded: %s", alt_url.c_str());
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }

            if (!data.empty()) {
                local_storage->store_data(entry.folder, entry.filename, &data[0], (unsigned int)data.size());
                ++downloaded;
                sce_assets_downloaded = downloaded;
                if (tidx >= 0) { ++sce_type_progress[tidx].downloaded; ++sce_type_progress[tidx].current; }
                PRINT_DEBUG("SceAssets: saved %s (%zu bytes)", entry.filename.c_str(), data.size());
            } else {
                PRINT_DEBUG("SceAssets: all CDN attempts failed for %s", entry.url.c_str());
            }

            // rate-limit: 200ms between requests to avoid hammering the CDN
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        PRINT_DEBUG("SceAssets: done — %u downloaded, %u skipped of %zu total", downloaded, skipped, queue.size());

        {
            std::lock_guard<std::recursive_mutex> lock(global_mutex);
            sce_assets_downloading = false;
        }

        if (overlay) overlay->NotifySceAssetsReady(downloaded, skipped, (uint32_t)queue.size());

    }).detach();
}


// Parse the SteamCardExchange game page HTML into a structured catalog of all series,
// cards, foil cards, badges, foil badges, booster packs, emoticons and backgrounds.
// Uses only std::string::find / rfind — no external HTML parser required.
Steam_User_Stats::SceGameData Steam_User_Stats::ParseSteamCardExchangeHtml(const std::string &html, uint32 app_id)
{
    SceGameData data{};
    data.appid = app_id;

    // ---- helpers (all capture html by ref) ----

    // Extract the value of attr="..." searching forward from 'from', stop at 'limit'.
    auto attr_val = [&](size_t from, size_t limit, const char *attr) -> std::string {
        std::string needle = std::string(attr) + "=\"";
        size_t p = html.find(needle, from);
        if (p == std::string::npos || p >= limit) return {};
        p += needle.size();
        size_t e = html.find('"', p);
        if (e == std::string::npos || e > limit) return {};
        return html.substr(p, e - p);
    };

    // Extract inner text of the next tag starting at 'from' (reads between > and <).
    auto tag_text = [&](size_t from, size_t limit) -> std::string {
        size_t gt = html.find('>', from);
        if (gt == std::string::npos || gt >= limit) return {};
        gt++;
        size_t lt = html.find('<', gt);
        if (lt == std::string::npos || lt > limit) return {};
        return html.substr(gt, lt - gt);
    };

    // URL-decode a percent-encoded string.
    auto url_decode = [](const std::string &src) -> std::string {
        std::string out;
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '%' && i + 2 < src.size()) {
                int v = 0;
                sscanf(src.c_str() + i + 1, "%2x", &v);
                out += (char)v;
                i += 2;
            } else if (src[i] == '+') {
                out += ' ';
            } else {
                out += src[i];
            }
        }
        return out;
    };

    // Find the href="..." of the <a> tag whose class contains 'anchor_pos',
    // searching backward within [min_pos, anchor_pos].
    auto href_before = [&](size_t anchor_pos, size_t min_pos) -> std::string {
        size_t hp = html.rfind("href=\"", anchor_pos);
        if (hp == std::string::npos || hp < min_pos) return {};
        hp += 6;
        size_t he = html.find('"', hp);
        if (he == std::string::npos) return {};
        return html.substr(hp, he - hp);
    };

    // Extract the steam market hash name from a /market/listings/753/... URL.
    auto market_hash = [&](const std::string &url) -> std::string {
        const char *marker = "/market/listings/753/";
        size_t p = url.find(marker);
        if (p == std::string::npos) return {};
        return url_decode(url.substr(p + strlen(marker)));
    };

    // Find the last occurrence of needle in html[min_pos, limit).
    auto rfind_in = [&](const std::string &needle, size_t min_pos, size_t limit) -> size_t {
        size_t last = std::string::npos;
        size_t p = min_pos;
        while (true) {
            size_t f = html.find(needle, p);
            if (f == std::string::npos || f >= limit) break;
            last = f;
            p = f + 1;
        }
        return last;
    };

    // ---- parse all series ----
    const std::string SERIES_PFX = "id=\"series-";
    // Item card div is identified by this unique class combination.
    const std::string ITEM_DIV   = "class=\"flex flex-col items-center p-5 gap-y-2 bg-gray-light\"";

    size_t pos = 0;
    while (true) {
        size_t sp = html.find(SERIES_PFX, pos);
        if (sp == std::string::npos) break;

        size_t id_s = sp + SERIES_PFX.size();
        size_t id_e = html.find('"', id_s);
        if (id_e == std::string::npos) break;
        std::string id_val = html.substr(id_s, id_e - id_s);

        // Top-level series anchor: id="series-N" where N is purely numeric.
        bool top_level = !id_val.empty();
        for (char c : id_val) if (!isdigit((unsigned char)c)) { top_level = false; break; }

        if (top_level) {
            int ser_num = std::stoi(id_val);

            // Find the end of this series block: start of the next top-level series anchor.
            size_t ser_end = html.size();
            size_t sf = id_e + 1;
            while (true) {
                size_t nsp = html.find(SERIES_PFX, sf);
                if (nsp == std::string::npos) break;
                size_t ns_s = nsp + SERIES_PFX.size();
                size_t ns_e = html.find('"', ns_s);
                if (ns_e == std::string::npos) break;
                std::string nid = html.substr(ns_s, ns_e - ns_s);
                bool ntop = !nid.empty();
                for (char c : nid) if (!isdigit((unsigned char)c)) { ntop = false; break; }
                if (ntop) { ser_end = nsp; break; }
                sf = ns_e + 1;
            }

            SceSeries series{};
            series.series_number = ser_num;
            series.series_name   = "Series " + std::to_string(ser_num);

            // ---- find all sections within this series block ----
            std::string sec_pfx = "id=\"series-" + std::to_string(ser_num) + "-";
            size_t sec_search = sp;

            while (sec_search < ser_end) {
                size_t secp = html.find(sec_pfx, sec_search);
                if (secp == std::string::npos || secp >= ser_end) break;

                size_t st_s = secp + sec_pfx.size();
                size_t st_e = html.find('"', st_s);
                if (st_e == std::string::npos) break;
                std::string sec_type = html.substr(st_s, st_e - st_s);

                // Section ends at the next section or series anchor.
                size_t sec_end = ser_end;
                {
                    size_t ns = html.find(sec_pfx, st_e + 1);
                    if (ns != std::string::npos && ns < sec_end) sec_end = ns;
                    size_t ts = html.find(SERIES_PFX, st_e + 1);
                    if (ts != std::string::npos && ts < sec_end) sec_end = ts;
                }

                SceItemType itype;
                bool known = true;
                if      (sec_type == "cards")                    itype = SceItemType::TradingCard;
                else if (sec_type == "foilcards")                itype = SceItemType::FoilCard;
                else if (sec_type == "booster")                  itype = SceItemType::BoosterPack;
                else if (sec_type == "badges")                   itype = SceItemType::Badge;
                else if (sec_type == "foilbadges")               itype = SceItemType::FoilBadge;
                else if (sec_type == "emoticons")                itype = SceItemType::Emoticon;
                else if (sec_type == "backgrounds")              itype = SceItemType::Background;
                else if (sec_type == "animatedbackgrounds")      itype = SceItemType::AnimatedBackground;
                else if (sec_type == "animatedminibackgrounds")  itype = SceItemType::AnimatedMiniBackground;
                else if (sec_type == "profiles")                 itype = SceItemType::Profile;
                else if (sec_type == "avatarframes")             itype = SceItemType::AvatarFrame;
                else if (sec_type == "avataranimated")           itype = SceItemType::AnimatedAvatar;
                else if (sec_type == "animatedstickers")         itype = SceItemType::AnimatedSticker;
                else if (sec_type == "startupmovies")            itype = SceItemType::StartupMovie;
                else                                             known = false;

                if (known) {
                    size_t item_search = secp;
                    while (item_search < sec_end) {
                        size_t ip = html.find(ITEM_DIV, item_search);
                        if (ip == std::string::npos || ip >= sec_end) break;

                        // Item block ends at the next item block or section end.
                        size_t ni = html.find(ITEM_DIV, ip + ITEM_DIV.size());
                        size_t ie = (ni != std::string::npos && ni < sec_end) ? ni : sec_end;

                        SceItem item{};
                        item.type   = itype;
                        item.series = ser_num;

                        if (itype == SceItemType::TradingCard || itype == SceItemType::FoilCard) {
                            // data-gallery-desc="Series N - Card M of T - NAME"
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Card %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);

                                // icon_url = src of the <img> that owns data-gallery-desc
                                size_t img_s = html.rfind("<img", dgp);
                                if (img_s != std::string::npos && img_s > ip) {
                                    size_t img_e = html.find('>', dgp);
                                    size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                    item.icon_url = attr_val(img_s, lim, "src");
                                }
                            }
                            // wallpaper = href of the gallery-src link
                            size_t gsc = html.find("gallery-src\"", ip);
                            if (gsc != std::string::npos && gsc < ie)
                                item.wallpaper_url = href_before(gsc, ip);

                            // market link + price = last btn-primary in block
                            size_t lbp = rfind_in("btn-primary\"", ip, ie);
                            if (lbp != std::string::npos) {
                                item.market_hash_name = market_hash(href_before(lbp, ip));
                                item.price_text       = tag_text(lbp, ie);
                            }

                        } else if (itype == SceItemType::BoosterPack) {
                            item.name = "Booster Pack";
                            // icon = boosterpack img src
                            size_t bpp = html.find("boosterpack/", ip);
                            if (bpp != std::string::npos && bpp < ie) {
                                size_t srcp = html.rfind("src=\"", bpp);
                                if (srcp != std::string::npos && srcp > ip) {
                                    srcp += 5;
                                    size_t srce = html.find('"', srcp);
                                    if (srce != std::string::npos) item.icon_url = html.substr(srcp, srce - srcp);
                                }
                            }
                            size_t lbp = rfind_in("btn-primary\"", ip, ie);
                            if (lbp != std::string::npos) {
                                item.market_hash_name = market_hash(href_before(lbp, ip));
                                item.price_text       = tag_text(lbp, ie);
                            }

                        } else if (itype == SceItemType::Badge || itype == SceItemType::FoilBadge) {
                            // first <img> is the badge icon; alt="Series N - NAME"
                            size_t img_p = html.find("<img", ip);
                            if (img_p != std::string::npos && img_p < ie) {
                                size_t img_e = html.find('>', img_p);
                                size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                item.icon_url = attr_val(img_p, lim, "src");
                                std::string alt = attr_val(img_p, lim, "alt");
                                size_t dash = alt.find(" - ");
                                if (dash != std::string::npos) item.name = alt.substr(dash + 3);
                            }
                            size_t lp = html.find("Level ", ip);
                            if (lp != std::string::npos && lp < ie)
                                sscanf(html.c_str() + lp + 6, "%d", &item.badge_level);
                            size_t xp = html.find("XP: ", ip);
                            if (xp != std::string::npos && xp < ie)
                                sscanf(html.c_str() + xp + 4, "%d", &item.badge_xp);

                        } else if (itype == SceItemType::Emoticon) {
                            // first <img> = animated preview; second <img> = static icon
                            size_t img1 = html.find("<img", ip);
                            if (img1 != std::string::npos && img1 < ie) {
                                size_t img1e = html.find('>', img1);
                                size_t lim1  = img1e != std::string::npos ? img1e + 1 : ie;
                                item.animated_url = attr_val(img1, lim1, "src");
                                std::string alt1  = attr_val(img1, lim1, "alt");
                                // alt1 = ":NAME: Chat Preview"
                                size_t cp = alt1.find(" Chat Preview");
                                if (cp != std::string::npos) alt1.resize(cp);
                                if (alt1.size() >= 2 && alt1.front() == ':') {
                                    size_t cl = alt1.rfind(':');
                                    if (cl > 0) item.emoticon_name = alt1.substr(1, cl - 1);
                                }
                                size_t img2 = html.find("<img", img1 + 1);
                                if (img2 != std::string::npos && img2 < ie) {
                                    size_t img2e = html.find('>', img2);
                                    item.icon_url = attr_val(img2, img2e != std::string::npos ? img2e + 1 : ie, "src");
                                }
                            }
                            // name from break-all div
                            size_t na = html.find("break-all\"", ip);
                            if (na != std::string::npos && na < ie) item.name = tag_text(na, ie);
                            // rarity from text-rarity-* class
                            size_t ra = html.find("text-rarity-", ip);
                            if (ra != std::string::npos && ra < ie) item.rarity = tag_text(ra, ie);
                            // market + price
                            size_t lbp = rfind_in("btn-primary\"", ip, ie);
                            if (lbp != std::string::npos) {
                                item.market_hash_name = market_hash(href_before(lbp, ip));
                                item.price_text       = tag_text(lbp, ie);
                            }

                        } else if (itype == SceItemType::Background) {
                            // wallpaper = gallery-src href
                            size_t gsc = html.find("gallery-src\"", ip);
                            if (gsc != std::string::npos && gsc < ie)
                                item.wallpaper_url = href_before(gsc, ip);

                            // data-gallery-desc="Series N - Background M of T - NAME"
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Background %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);

                                // icon_url = img src (the thumbnail; strip ?size=... query)
                                size_t img_s = html.rfind("<img", dgp);
                                if (img_s != std::string::npos && img_s > ip) {
                                    size_t img_e = html.find('>', dgp);
                                    size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                    std::string src = attr_val(img_s, lim, "src");
                                    size_t q = src.find('?');
                                    if (q != std::string::npos) src.resize(q);
                                    item.icon_url = src;
                                }
                            }
                            // rarity
                            size_t ra = html.find("text-rarity-", ip);
                            if (ra != std::string::npos && ra < ie) item.rarity = tag_text(ra, ie);
                            // market + price = last btn-primary (Preview link comes first)
                            size_t lbp = rfind_in("btn-primary\"", ip, ie);
                            if (lbp != std::string::npos) {
                                std::string murl = href_before(lbp, ip);
                                if (murl.find("/market/listings/") != std::string::npos) {
                                    item.market_hash_name = market_hash(murl);
                                    item.price_text       = tag_text(lbp, ie);
                                }
                            }

                        } else if (itype == SceItemType::AnimatedBackground ||
                                   itype == SceItemType::AnimatedMiniBackground) {
                            // data-gallery-desc="Series N - Animated Background M of T - NAME"
                            // or                "Series N - Animated Mini Background M of T - NAME"
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);

                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Animated Background %d of %d", &sn, &slot, &tot) == 3 ||
                                    sscanf(desc.c_str(), "Series %d - Animated Mini Background %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                            }
                            // video-src link (first link in top row = mp4)
                            size_t gvs = html.find("gallery-video-src\"", ip);
                            if (gvs != std::string::npos && gvs < ie)
                                item.video_mp4_url = href_before(gvs, ip);
                            // wallpaper link (second link in top row)
                            {
                                size_t wp_search = (gvs != std::string::npos) ? gvs + 1 : ip;
                                size_t hp2 = html.find("href=\"", wp_search);
                                if (hp2 != std::string::npos && hp2 < ie) {
                                    hp2 += 6;
                                    size_t he2 = html.find('"', hp2);
                                    if (he2 != std::string::npos && he2 < ie) {
                                        std::string u = html.substr(hp2, he2 - hp2);
                                        if (u.find("profilebackground") != std::string::npos ||
                                            u.find(".jpg") != std::string::npos)
                                            item.wallpaper_url = u;
                                    }
                                }
                            }
                            // static thumbnail img src (hover-toggle-primary > img)
                            {
                                size_t htp = html.find("hover-toggle-primary", ip);
                                if (htp != std::string::npos && htp < ie) {
                                    size_t img_p = html.find("<img", htp);
                                    if (img_p != std::string::npos && img_p < ie) {
                                        size_t img_e = html.find('>', img_p);
                                        size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                        std::string src = attr_val(img_p, lim, "src");
                                        size_t q = src.find('?');
                                        if (q != std::string::npos) src.resize(q);
                                        item.static_img_url = src;
                                    }
                                }
                            }
                            // webm source
                            {
                                size_t sp2 = html.find("video/webm", ip);
                                if (sp2 != std::string::npos && sp2 < ie) {
                                    size_t srcp = html.rfind("src=\"", sp2);
                                    if (srcp != std::string::npos && srcp > ip) {
                                        srcp += 5;
                                        size_t srce = html.find('"', srcp);
                                        if (srce != std::string::npos) item.video_webm_url = html.substr(srcp, srce - srcp);
                                    }
                                }
                            }
                            item.rarity = tag_text(html.find("text-rarity-", ip), ie);

                        } else if (itype == SceItemType::Profile) {
                            // simple static image, no market link — points shop only
                            size_t img_p = html.find("<img", ip);
                            if (img_p != std::string::npos && img_p < ie) {
                                size_t img_e = html.find('>', img_p);
                                size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                item.icon_url = attr_val(img_p, lim, "src");
                                item.name     = attr_val(img_p, lim, "alt");
                            }
                            item.rarity = tag_text(html.find("text-rarity-", ip), ie);
                            // Steam Points price (inside btn-primary with steam-points.svg)
                            size_t spp = html.find("steam-points.svg", ip);
                            if (spp != std::string::npos && spp < ie) {
                                size_t sp2 = html.find("<span>", spp);
                                if (sp2 != std::string::npos && sp2 < ie) {
                                    sp2 += 6;
                                    size_t sp3 = html.find("</span>", sp2);
                                    if (sp3 != std::string::npos && sp3 < ie) item.points_price = html.substr(sp2, sp3 - sp2);
                                }
                            }
                            // preview URL (first btn-primary href in block)
                            size_t pbp = html.find("btn-primary\"", ip);
                            if (pbp != std::string::npos && pbp < ie)
                                item.preview_url = href_before(pbp, ip);

                        } else if (itype == SceItemType::AvatarFrame) {
                            // data-gallery-desc="Series N - Avatar Frame M of T - NAME"
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);
                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Avatar Frame %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                            }
                            // gallery-src link = animated PNG
                            size_t gsc = html.find("gallery-src\"", ip);
                            if (gsc != std::string::npos && gsc < ie)
                                item.wallpaper_url = href_before(gsc, ip);  // animated variant
                            // static img from hover-toggle-primary
                            {
                                size_t htp = html.find("hover-toggle-primary", ip);
                                if (htp != std::string::npos && htp < ie) {
                                    size_t img_p = html.find("<img", htp);
                                    if (img_p != std::string::npos && img_p < ie) {
                                        size_t img_e = html.find('>', img_p);
                                        size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                        item.static_img_url = attr_val(img_p, lim, "src");
                                    }
                                }
                            }
                            item.rarity = tag_text(html.find("text-rarity-", ip), ie);

                        } else if (itype == SceItemType::AnimatedAvatar) {
                            // data-gallery-desc="Series N - Animated Avatar M of T - NAME"
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);
                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Animated Avatar %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                            }
                            // gallery-src link = .gif animation
                            size_t gsc = html.find("gallery-src\"", ip);
                            if (gsc != std::string::npos && gsc < ie)
                                item.video_mp4_url = href_before(gsc, ip);  // reuse mp4 field for gif
                            // static img from hover-toggle-primary
                            {
                                size_t htp = html.find("hover-toggle-primary", ip);
                                if (htp != std::string::npos && htp < ie) {
                                    size_t img_p = html.find("<img", htp);
                                    if (img_p != std::string::npos && img_p < ie) {
                                        size_t img_e = html.find('>', img_p);
                                        size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                        item.static_img_url = attr_val(img_p, lim, "src");
                                    }
                                }
                            }
                            // animated gif from hover-toggle-secondary
                            {
                                size_t hts = html.find("hover-toggle-secondary", ip);
                                if (hts != std::string::npos && hts < ie) {
                                    size_t img_p = html.find("<img", hts);
                                    if (img_p != std::string::npos && img_p < ie) {
                                        size_t img_e = html.find('>', img_p);
                                        size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                        item.icon_url = attr_val(img_p, lim, "src");  // gif shown on hover
                                    }
                                }
                            }
                            item.rarity = tag_text(html.find("text-rarity-", ip), ie);
                        } else if (itype == SceItemType::AnimatedSticker) {
                            // data-gallery-desc="Series N - Animated Sticker M of T - NAME"
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);
                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Animated Sticker %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                            }
                            // gallery-src link = animated PNG
                            size_t gsc = html.find("gallery-src\"", ip);
                            if (gsc != std::string::npos && gsc < ie)
                                item.wallpaper_url = href_before(gsc, ip);
                            // static img from hover-toggle-primary → stored in icon_url for thumbnail display
                            {
                                size_t htp = html.find("hover-toggle-primary", ip);
                                if (htp != std::string::npos && htp < ie) {
                                    size_t img_p = html.find("<img", htp);
                                    if (img_p != std::string::npos && img_p < ie) {
                                        size_t img_e = html.find('>', img_p);
                                        size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                        item.icon_url = attr_val(img_p, lim, "src");
                                    }
                                }
                            }
                            item.rarity = tag_text(html.find("text-rarity-", ip), ie);
                        } else if (itype == SceItemType::StartupMovie) {
                            // data-gallery-desc="Series N - Startup Movie M of T - NAME" (inside hover-toggle-secondary)
                            size_t dgp = html.find("data-gallery-desc", ip);
                            if (dgp != std::string::npos && dgp < ie) {
                                std::string desc = attr_val(dgp, ie, "data-gallery-desc");
                                size_t dash = desc.rfind(" - ");
                                if (dash != std::string::npos) item.name = desc.substr(dash + 3);
                                int sn = 0, slot = 0, tot = 0;
                                if (sscanf(desc.c_str(), "Series %d - Startup Movie %d of %d", &sn, &slot, &tot) == 3) {
                                    item.slot  = slot;
                                    item.total = tot;
                                }
                            }
                            // gallery-video-src link = .webm
                            size_t gvs = html.find("gallery-video-src\"", ip);
                            if (gvs != std::string::npos && gvs < ie)
                                item.video_webm_url = href_before(gvs, ip);
                            // static thumbnail img from hover-toggle-primary → icon_url (thumbnail for browser)
                            {
                                size_t htp = html.find("hover-toggle-primary", ip);
                                if (htp != std::string::npos && htp < ie) {
                                    size_t img_p = html.find("<img", htp);
                                    if (img_p != std::string::npos && img_p < ie) {
                                        size_t img_e = html.find('>', img_p);
                                        size_t lim   = img_e != std::string::npos ? img_e + 1 : ie;
                                        item.icon_url = attr_val(img_p, lim, "src");
                                    }
                                }
                            }
                            item.rarity = tag_text(html.find("text-rarity-", ip), ie);
                        }

                        if (!item.name.empty())
                            series.items.push_back(std::move(item));

                        item_search = ip + ITEM_DIV.size();
                    }
                }

                sec_search = st_e + 1;
            }

            if (!series.items.empty())
                data.series.push_back(std::move(series));
        }

        pos = id_e + 1;
    }

    return data;
}


// Get the info on the most achieved achievement for the game, returns an iterator index you can use to fetch
// the next most achieved afterwards.  Will return -1 if there is no data on achievement 
// percentages (ie, you haven't called RequestGlobalAchievementPercentages and waited on the callback).
int Steam_User_Stats::GetMostAchievedAchievementInfo( char *pchName, uint32 unNameBufLen, float *pflPercent, bool *pbAchieved )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (!pchName) return -1;

    if (!global_achievement_percentages_populated) return -1;
    if (sorted_global_achievement_percentages.empty()) return -1;

    const auto &entry = sorted_global_achievement_percentages[0];
    const std::string &name = entry.first;

    if (pchName && unNameBufLen) {
        memset(pchName, 0, unNameBufLen);
        name.copy(pchName, unNameBufLen - 1);
    }

    if (pflPercent) *pflPercent = entry.second;
    if (pbAchieved) {
        bool achieved = false;
        GetAchievement(name.c_str(), &achieved);
        *pbAchieved = achieved;
    }

    return 0;
}


// Get the info on the next most achieved achievement for the game. Call this after GetMostAchievedAchievementInfo or another
// GetNextMostAchievedAchievementInfo call passing the iterator from the previous call. Returns -1 after the last
// achievement has been iterated.
int Steam_User_Stats::GetNextMostAchievedAchievementInfo( int iIteratorPrevious, char *pchName, uint32 unNameBufLen, float *pflPercent, bool *pbAchieved )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (iIteratorPrevious < 0) return -1;

    if (!global_achievement_percentages_populated) return -1;

    unsigned iIteratorCurrent = static_cast<unsigned>(iIteratorPrevious + 1);
    if (iIteratorCurrent >= sorted_global_achievement_percentages.size()) return -1;

    const auto &entry = sorted_global_achievement_percentages[iIteratorCurrent];
    const std::string &name = entry.first;

    if (pchName && unNameBufLen) {
        memset(pchName, 0, unNameBufLen);
        name.copy(pchName, unNameBufLen - 1);
    }

    if (pflPercent) *pflPercent = entry.second;
    if (pbAchieved) {
        bool achieved = false;
        GetAchievement(name.c_str(), &achieved);
        *pbAchieved = achieved;
    }

    return static_cast<int>(iIteratorCurrent);
}


// Returns the percentage of users who have achieved the specified achievement.
bool Steam_User_Stats::GetAchievementAchievedPercent( const char *pchName, float *pflPercent )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    auto it = defined_achievements_find(pchName);
    if (defined_achievements.end() == it) return false;

    if (pflPercent) {
        if (global_achievement_percentages_populated) {
            auto git = global_achievement_percentages.find(pchName);
            if (git != global_achievement_percentages.end()) {
                *pflPercent = git->second;
            } else {
                *pflPercent = 0.0f;
            }
        } else {
            size_t idx = it - defined_achievements.begin();
            *pflPercent = (float)(90 * (defined_achievements.size() - idx) / defined_achievements.size());
        }
    }

    return true;
}


// For achievements that have related Progress stats, use this to query what the bounds of that progress are.
// You may want this info to selectively call IndicateAchievementProgress when appropriate milestones of progress
// have been made, to show a progress notification to the user.
bool Steam_User_Stats::GetAchievementProgressLimits( const char *pchName, int32 *pnMinProgress, int32 *pnMaxProgress )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    float fMinProgress{};
    float fMaxProgress{};
    bool ret = GetAchievementProgressLimits(pchName, &fMinProgress, &fMaxProgress);
    if (ret) {
        if (pnMinProgress) *pnMinProgress = static_cast<int32>(fMinProgress);
        if (pnMaxProgress) *pnMaxProgress = static_cast<int32>(fMaxProgress);
    }
    return ret;
}

bool Steam_User_Stats::GetAchievementProgressLimits( const char *pchName, float *pfMinProgress, float *pfMaxProgress )
{
    PRINT_DEBUG("'%s'", pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    if (!pchName) return false;

    auto it = defined_achievements.end();
    try {
        it = defined_achievements_find(pchName);
    }
    catch (...) {}
    if (defined_achievements.end() == it) return false;

    if (pfMinProgress) *pfMinProgress = 0;
    if (pfMaxProgress) *pfMaxProgress = 0;

    try {
        std::string pch_name = it->value("name", std::string());
        auto ach = user_achievements.find(pch_name);
        if (user_achievements.end() != ach) {
            auto it_progress = ach->find("progress");
            auto it_max_progress = ach->find("max_progress");
            if (ach->end() == it_progress || ach->end() == it_max_progress) return false;

            if (pfMinProgress) {
                try {
                    if (it_progress->is_number()) {
                        *pfMinProgress = it_progress->get<float>();
                    } else {
                        auto s_ptr = it_progress->get_ptr<std::string*>();
                        if (s_ptr) {
                            *pfMinProgress  = std::stof(*s_ptr);
                        }
                    }
                }catch(...){}
            }
            if (pfMaxProgress) {
                try {
                    if (it_max_progress->is_number()) {
                        *pfMaxProgress = it_max_progress->get<float>();
                    } else {
                        auto s_ptr = it_max_progress->get_ptr<std::string*>();
                        if (s_ptr) {
                            *pfMaxProgress  = std::stof(*s_ptr);
                        }
                    }
                }catch(...){}
            }
            return true;
        }
    }
    catch (...) {}

    return false;
}



// --- steam callbacks

void Steam_User_Stats::load_achievements_icons()
{
    if (last_loaded_ach_icon >= defined_achievements.size() || settings->paginated_achievements_icons <= 0) return;

#ifndef EMU_RELEASE_BUILD
    auto now1 = std::chrono::high_resolution_clock::now();
#endif

    int idx = 0;
    for (;
        idx < settings->paginated_achievements_icons && last_loaded_ach_icon < defined_achievements.size();
        ++idx, ++last_loaded_ach_icon) {
        auto &ach = defined_achievements.at(last_loaded_ach_icon);
        load_ach_icon(ach, true);
        load_ach_icon(ach, false);
    }

#ifndef EMU_RELEASE_BUILD
    auto now2 = std::chrono::high_resolution_clock::now();
    auto dd = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
    PRINT_DEBUG("attempted to load %d achievements icons in %u ms", idx * 2, dd);
#endif

}
