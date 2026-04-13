#include "overlay/steam_overlay_stats.h"
// translation
#include "overlay/steam_overlay_translations.h"
#include <utility>
#include <algorithm>
#include <cmath>

// EMA smoothing factor (0..1). Lower = smoother but slower to react.
static constexpr float EMA_ALPHA = 0.1f;

Steam_Overlay_Stats::Steam_Overlay_Stats(class Settings* settings):
    settings(settings)
{
    show_fps = settings->overlay_always_show_fps;
    show_frametime = settings->overlay_always_show_frametime;
    show_playtime = settings->overlay_always_show_playtime;
}

bool Steam_Overlay_Stats::show_any_stats() const
{
    return show_fps || show_frametime || show_playtime;
}

void Steam_Overlay_Stats::update_frametime(const std::chrono::steady_clock::time_point &now)
{
    float dt_ms = std::chrono::duration<float, std::milli>(now - last_frame_timepoint).count();
    last_frame_timepoint = now;

    // Clamp absurd values (e.g. first frame, or debugger pause)
    if (dt_ms < 0.0f) dt_ms = 0.0f;
    if (dt_ms > 1000.0f) dt_ms = 1000.0f;

    // Store in ring buffer
    frametime_history[frametime_history_idx] = dt_ms;
    frametime_history_idx = (frametime_history_idx + 1) % FRAMETIME_HISTORY_SIZE;
    if (frametime_history_count < FRAMETIME_HISTORY_SIZE) frametime_history_count++;

    // EMA smoothing
    if (smoothed_frametime_ms <= 0.0f) {
        smoothed_frametime_ms = dt_ms; // seed on first frame
    } else {
        smoothed_frametime_ms = EMA_ALPHA * dt_ms + (1.0f - EMA_ALPHA) * smoothed_frametime_ms;
    }
    smoothed_fps = (smoothed_frametime_ms > 0.0f) ? (1000.0f / smoothed_frametime_ms) : 0.0f;

    // Min/max/avg over ring buffer
    float sum = 0.0f;
    float mn = 1e9f;
    float mx = 0.0f;
    for (int i = 0; i < frametime_history_count; i++) {
        float v = frametime_history[i];
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    avg_frametime_ms = sum / frametime_history_count;
    min_frametime_ms = mn;
    max_frametime_ms = mx;

    // Snapshot display values every 500ms
    auto display_elapsed = std::chrono::duration<float, std::milli>(now - last_display_update).count();
    if (display_elapsed >= 500.0f || display_fps <= 0.0f) {
        last_display_update = now;
        display_fps = smoothed_fps;
        display_frametime_ms = smoothed_frametime_ms;
        display_min_ft = min_frametime_ms;
        display_max_ft = max_frametime_ms;
        display_avg_ft = avg_frametime_ms;
    }
}

void Steam_Overlay_Stats::update_playtime(const std::chrono::steady_clock::time_point &now)
{
    const auto update_duration_sec = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_playtime
    ).count();
    if (update_duration_sec < 1) return;

    last_playtime = now;

    const auto time_duration_sec = (unsigned long long)std::chrono::duration_cast<std::chrono::seconds>(
        now - initial_time
    ).count();
    active_playtime_sec = static_cast<unsigned>(time_duration_sec % 60);

    const auto time_duration_min = time_duration_sec / 60;
    active_playtime_min = static_cast<unsigned>(time_duration_min % 60);

    const auto time_duration_hr = time_duration_min / 60;
    active_playtime_hr = static_cast<unsigned>(time_duration_hr % 24);
}

void Steam_Overlay_Stats::render_stats(int current_language)
{
    auto now = std::chrono::steady_clock::now();
    if (show_fps || show_frametime) {
        update_frametime(now);
    }
    if (show_playtime) {
        update_playtime(now);
    }

    ImGui::PushFont(font, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, settings->overlay_appearance.notification_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(
        settings->overlay_appearance.stats_background_r,
        settings->overlay_appearance.stats_background_g,
        settings->overlay_appearance.stats_background_b,
        settings->overlay_appearance.stats_background_a
    ));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(
        settings->overlay_appearance.stats_text_r,
        settings->overlay_appearance.stats_text_g,
        settings->overlay_appearance.stats_text_b,
        settings->overlay_appearance.stats_text_a
    ));
    
    // Build the main stats text line
    std::stringstream stats_txt_buff{};
    if (show_fps) {
        stats_txt_buff << translationFpsDisplay[current_language]
                       << std::left << std::setw(4) << std::fixed << std::setprecision(1)
                       << display_fps
                       << std::defaultfloat << std::right << std::setw(0);
    }
    if (show_frametime) {
        if (stats_txt_buff.tellp() > 0) {
            stats_txt_buff << " | ";
        }
        stats_txt_buff << translationFrametimeDisplay[current_language]
                       << std::left << std::setw(5) << std::fixed << std::setprecision(1)
                       << display_frametime_ms
                       << std::defaultfloat << std::right << std::setw(0)
                       << translationFrametimeUnitDisplay[current_language];
    }
    if (show_playtime) {
        if (stats_txt_buff.tellp() > 0) {
            stats_txt_buff << " | ";
        }
        const auto org_fill = stats_txt_buff.fill();
        stats_txt_buff << translationPlaytimeDisplay[current_language]
                       << std::setw(2) << std::setfill('0')
                       << active_playtime_hr << ':'
                       << std::setw(2) << std::setfill('0')
                       << active_playtime_min << ':'
                       << std::setw(2) << std::setfill('0')
                       << active_playtime_sec
                       << std::setw(0) << std::setfill(org_fill);
    }
    const auto stats_txt = stats_txt_buff.str();

    // Determine if we need graph space
    bool show_graph = (show_fps || show_frametime) && frametime_history_count > 1;
    float graph_height = 40.0f;

    // Pre-compute sorted frametimes for percentiles (sort a copy of the ring buffer)
    float sorted_ft[FRAMETIME_HISTORY_SIZE];
    int count = frametime_history_count;
    if (show_graph) {
        int ring_start = (frametime_history_idx - count + FRAMETIME_HISTORY_SIZE) % FRAMETIME_HISTORY_SIZE;
        for (int i = 0; i < count; i++)
            sorted_ft[i] = frametime_history[(ring_start + i) % FRAMETIME_HISTORY_SIZE];
        std::sort(sorted_ft, sorted_ft + count); // ascending: lowest frametime first
    }

    // Determine minimum content width for graphs
    float min_content_width = show_graph ? 260.0f : 0.0f;

    auto &io = ImGui::GetIO();
    const float pos_x_norm = settings->overlay_stats_pos_x;
    const float pos_y_norm = settings->overlay_stats_pos_y;

    if (ImGui::Begin("wnd_fps_frametime", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse)) {

        // Anchor the window position based on its actual size
        ImVec2 win_sz = ImGui::GetWindowSize();
        float anchor_x = win_sz.x * pos_x_norm;
        float anchor_y = win_sz.y * pos_y_norm;
        ImGui::SetWindowPos(ImVec2(
            io.DisplaySize.x * pos_x_norm - anchor_x,
            io.DisplaySize.y * pos_y_norm - anchor_y
        ));

        float content_width = ImGui::GetContentRegionAvail().x;
        if (content_width < min_content_width) content_width = min_content_width;

        ImGui::TextWrapped("%s", stats_txt.c_str());

        if (show_graph) {
            // Build chronological arrays from ring buffer
            float ft_data[FRAMETIME_HISTORY_SIZE];
            float fps_data[FRAMETIME_HISTORY_SIZE];
            int ring_start = (frametime_history_idx - count + FRAMETIME_HISTORY_SIZE) % FRAMETIME_HISTORY_SIZE;
            for (int i = 0; i < count; i++) {
                float ft = frametime_history[(ring_start + i) % FRAMETIME_HISTORY_SIZE];
                ft_data[i] = ft;
                fps_data[i] = (ft > 0.0f) ? (1000.0f / ft) : 0.0f;
            }

            // Compute FPS percentiles from sorted frametimes (ascending ft = descending fps)
            // 1% low fps = fps at the 99th percentile frametime
            auto ft_percentile = [&](float pct) -> float {
                int idx = (int)(pct * count) - 1;
                if (idx < 0) idx = 0;
                if (idx >= count) idx = count - 1;
                return sorted_ft[idx]; // ascending order
            };
            // For FPS lows: the worst X% of frames have the highest frametimes
            auto fps_low = [&](float pct) -> float {
                // pct = 0.01 means 1% low: average of the worst 1% frametimes, converted to FPS
                int n = (int)(pct * count);
                if (n < 1) n = 1;
                float sum = 0.0f;
                for (int i = count - n; i < count; i++) sum += sorted_ft[i]; // highest frametimes
                float avg_ft = sum / n;
                return (avg_ft > 0.0f) ? (1000.0f / avg_ft) : 0.0f;
            };

            float fps_1_low = fps_low(0.01f);
            float fps_5_low = fps_low(0.05f);

            // FPS min/max/avg (use display_ values for stable text)
            float fps_min = (display_max_ft > 0.0f) ? (1000.0f / display_max_ft) : 0.0f;
            float fps_max = (display_min_ft > 0.0f) ? (1000.0f / display_min_ft) : 0.0f;
            float fps_avg = (display_avg_ft > 0.0f) ? (1000.0f / display_avg_ft) : 0.0f;

            // Frametime percentiles (highest = worst)
            float ft_99 = ft_percentile(0.99f); // 1% worst
            float ft_95 = ft_percentile(0.95f); // 5% worst

            // ---- Frametime graph ----
            if (show_frametime) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Frametime");

                float ft_scale_max = max_frametime_ms * 1.2f;
                if (ft_scale_max < 1.0f) ft_scale_max = 1.0f;

                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PlotLines("##ft_graph", ft_data, count, 0, nullptr,
                    0.0f, ft_scale_max, ImVec2(content_width, graph_height));
                ImGui::PopStyleColor(2);

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Min: %.1fms  Avg: %.1fms  Max: %.1fms",
                    display_min_ft, display_avg_ft, display_max_ft);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "1%% high: %.1fms  5%% high: %.1fms",
                    ft_99, ft_95);
            }

            // ---- FPS graph ----
            if (show_fps) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "FPS");

                float fps_scale_max = fps_max * 1.2f;
                if (fps_scale_max < 1.0f) fps_scale_max = 1.0f;

                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PlotLines("##fps_graph", fps_data, count, 0, nullptr,
                    0.0f, fps_scale_max, ImVec2(content_width, graph_height));
                ImGui::PopStyleColor(2);

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Min: %.0f  Avg: %.0f  Max: %.0f",
                    fps_min, fps_avg, fps_max);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "1%% Low: %.0f  5%% Low: %.0f",
                    fps_1_low, fps_5_low);
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}
