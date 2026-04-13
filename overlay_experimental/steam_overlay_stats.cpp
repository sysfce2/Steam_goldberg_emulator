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
                       << smoothed_fps
                       << std::defaultfloat << std::right << std::setw(0);
    }
    if (show_frametime) {
        if (stats_txt_buff.tellp() > 0) {
            stats_txt_buff << " | ";
        }
        stats_txt_buff << translationFrametimeDisplay[current_language]
                       << std::left << std::setw(5) << std::fixed << std::setprecision(1)
                       << smoothed_frametime_ms
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

    // Calculate window size
    const auto msg_box = ImGui::CalcTextSize(
        stats_txt.c_str(),
        stats_txt.c_str() + stats_txt.size()
    );
    auto &global_style = ImGui::GetStyle();
    const float padding_all_sides = global_style.WindowPadding.y + global_style.WindowPadding.x;

    // Min/max line (only when graph is shown)
    float min_max_line_h = 0.0f;
    float min_width = 0.0f;
    if (show_graph) {
        char min_max_buf[128];
        snprintf(min_max_buf, sizeof(min_max_buf), "Min: %.1fms  Avg: %.1fms  Max: %.1fms",
            min_frametime_ms, avg_frametime_ms, max_frametime_ms);
        ImVec2 mm_sz = ImGui::CalcTextSize(min_max_buf);
        min_max_line_h = mm_sz.y + 2.0f;
        min_width = mm_sz.x;
    }

    float content_width = msg_box.x;
    if (show_graph && min_width > content_width) content_width = min_width;
    if (show_graph && content_width < 200.0f) content_width = 200.0f; // min graph width

    float total_height = msg_box.y + padding_all_sides;
    if (show_graph) total_height += 4.0f + graph_height + 2.0f + min_max_line_h;

    const auto stats_box = ImVec2(content_width + padding_all_sides, total_height);
    ImGui::SetNextWindowSize(stats_box);

    auto &io = ImGui::GetIO();
    const auto anchor_point_x = stats_box.x * settings->overlay_stats_pos_x;
    const auto anchor_point_y = stats_box.y * settings->overlay_stats_pos_y;
    ImGui::SetNextWindowPos({
        io.DisplaySize.x * settings->overlay_stats_pos_x - anchor_point_x,
        io.DisplaySize.y * settings->overlay_stats_pos_y - anchor_point_y
    });

    if (ImGui::Begin("wnd_fps_frametime", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::TextWrapped("%s", stats_txt.c_str());

        // Frametime graph
        if (show_graph) {
            // Build a contiguous array in chronological order from the ring buffer
            float graph_data[FRAMETIME_HISTORY_SIZE];
            int count = frametime_history_count;
            int start = (frametime_history_idx - count + FRAMETIME_HISTORY_SIZE) % FRAMETIME_HISTORY_SIZE;
            for (int i = 0; i < count; i++) {
                graph_data[i] = frametime_history[(start + i) % FRAMETIME_HISTORY_SIZE];
            }

            // Scale: use max_frametime with a bit of headroom, min at 0
            float scale_max = max_frametime_ms * 1.2f;
            if (scale_max < 1.0f) scale_max = 1.0f;

            ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
            ImGui::PlotLines("##ft_graph", graph_data, count, 0, nullptr,
                0.0f, scale_max, ImVec2(content_width, graph_height));
            ImGui::PopStyleColor(2);

            // Min / Avg / Max line
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Min: %.1fms  Avg: %.1fms  Max: %.1fms",
                min_frametime_ms, avg_frametime_ms, max_frametime_ms);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}
