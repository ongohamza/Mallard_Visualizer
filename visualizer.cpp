#include <ncurses.h>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <random>
#include "config_parser.h"
#include "visualizer.h"

// VU Meter modes
enum VuMeterMode {
    VU_RMS,
    VU_PEAK
};
VuMeterMode vuMeterMode = VU_RMS;

// Helper function to select a color from the gradient based on amplitude
int selectColorByAmplitude(float amplitude_percent, const std::vector<int>& colorPairIDs) {
    if (colorPairIDs.empty()) return 1;
    amplitude_percent = std::max(0.0f, std::min(1.0f, amplitude_percent));
    int colorIdx = static_cast<int>(amplitude_percent * (colorPairIDs.size() - 1));
    return colorPairIDs[colorIdx];
}

// Global function to handle color fading in and out with a decay rate
int getFadedColorPairID(float currentAmplitude, float& lastDecay, const std::vector<int>& colorPairIDs, float decay_rate) {
    if (currentAmplitude > lastDecay) {
        lastDecay = std::min(currentAmplitude, lastDecay + decay_rate);
    } else {
        lastDecay = std::max(currentAmplitude, lastDecay - decay_rate);
    }
    return selectColorByAmplitude(lastDecay, colorPairIDs);
}

// Helper function for ray-segment intersection used in 'distort' mode
float getIntersectionDist(float ray_x, float ray_y, float p1x, float p1y, float p2x, float p2y) {
    float seg_x = p2x - p1x;
    float seg_y = p2y - p1y;
    float det = ray_x * seg_y - ray_y * seg_x;
    if (std::abs(det) < 1e-6) return -1.0f;
    float t = (p1x * seg_y - p1y * seg_x) / det;
    float u = (p1x * ray_y - p1y * ray_x) / det;
    if (t > 0 && u >= 0 && u <= 1) return t;
    return -1.0f;
}

void drawCustomShape(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, const CustomVisualizer& visualizer) {
    int centerX = width / 2;
    int centerY = height / 2;

    if (visualizer.type == ShapeVisualizerType::DISTORT) {
        if (visualizer.polygons.empty()) return;
        const float PI = 3.1415926535f;
        float scale = std::min(width, height) / 200.0f;
        for (int i = 0; i < BUFFER_FRAMES; ++i) {
            float mono_sample = (static_cast<float>(leftData[i]) + static_cast<float>(rightData[i])) / 2.0f;
            float radius = std::abs(mono_sample) / 32767.0f;
            float angle = (2.0f * PI * i) / BUFFER_FRAMES;
            
            float ray_x = std::cos(angle);
            float ray_y = std::sin(angle);
            float min_dist = -1.0f;
            for (const auto& polygon : visualizer.polygons) {
                for (size_t v = 0; v < polygon.size(); ++v) {
                    auto p1 = polygon[v];
                    auto p2 = polygon[(v + 1) % polygon.size()];
                    float dist = getIntersectionDist(ray_x, ray_y, p1.first, p1.second, p2.first, p2.second);
                    if (dist > 0 && (min_dist < 0 || dist < min_dist)) {
                        min_dist = dist;
                    }
                }
            }
            if (min_dist > 0) {
                float x = radius * min_dist * scale * ray_x;
                float y = radius * min_dist * scale * ray_y;
                int colorPairID = selectColorByAmplitude(radius, colorPairIDs);
                int screen_x = centerX + static_cast<int>(x);
                int screen_y = centerY + static_cast<int>(y * 0.6f);
                if (screen_x >= 0 && screen_x < width && screen_y >= 0 && screen_y < height) {
                    wattron(win, COLOR_PAIR(colorPairID));
                    mvwaddch(win, screen_y, screen_x, '.');
                    wattroff(win, COLOR_PAIR(colorPairID));
                }
            }
        }
    } else {
        size_t total_vertices = 0;
        for (const auto& poly : visualizer.polygons) total_vertices += poly.size();
        if (total_vertices < 2) return;
        const size_t points_per_vertex_side = 40;
        const size_t total_points = total_vertices * points_per_vertex_side;
        static std::vector<float> pointAmplitudes;
        if(pointAmplitudes.size() != total_points) pointAmplitudes.resize(total_points, 0.0f);
        const float rise_factor = 0.5f;
        for (size_t i = 0; i < total_points; ++i) {
            size_t start_idx = i * BUFFER_FRAMES / total_points;
            size_t end_idx = (i + 1) * BUFFER_FRAMES / total_points;
            if(end_idx > BUFFER_FRAMES) end_idx = BUFFER_FRAMES;
            float sum_sq = 0;
            int count = 0;
            for (size_t j = start_idx; j < end_idx; ++j) {
                float mono_sample = (static_cast<float>(leftData[j]) + static_cast<float>(rightData[j])) / 2.0f;
                sum_sq += mono_sample * mono_sample;
                count++;
            }
            float current_rms = (count > 0) ? sqrtf(sum_sq / count) / 32767.0f : 0.0f;
            if (current_rms > pointAmplitudes[i]) pointAmplitudes[i] += (current_rms - pointAmplitudes[i]) * rise_factor;
            else pointAmplitudes[i] = std::max(0.0f, pointAmplitudes[i] - decay__factor);
        }
        float scale = std::min(width, height) / 250.0f;
        size_t amplitude_idx_offset = 0;
        for (const auto& polygon : visualizer.polygons) {
            if (polygon.size() < 2) continue;
            for (size_t side = 0; side < polygon.size(); ++side) {
                auto p1 = polygon[side];
                auto p2 = polygon[(side + 1) % polygon.size()];
                for (size_t i = 0; i < points_per_vertex_side; ++i) {
                    float t = static_cast<float>(i) / points_per_vertex_side;
                    float base_x = p1.first + t * (p2.first - p1.first);
                    float base_y = p1.second + t * (p2.second - p1.second);
                    size_t amplitude_index = amplitude_idx_offset + side * points_per_vertex_side + i;
                    if(amplitude_index >= total_points) continue;
                    float amplitude = pointAmplitudes[amplitude_index];
                    float x = base_x * scale * (1.0f + amplitude * 0.5f);
                    float y = base_y * scale * (1.0f + amplitude * 0.5f);
                    int colorPairID = selectColorByAmplitude(amplitude, colorPairIDs);
                    int screen_x = centerX + static_cast<int>(x);
                    int screen_y = centerY + static_cast<int>(y * 0.6f);
                    if (screen_x >= 0 && screen_x < width && screen_y >= 0 && screen_y < height) {
                        wattron(win, COLOR_PAIR(colorPairID));
                        mvwaddch(win, screen_y, screen_x, '.');
                        wattroff(win, COLOR_PAIR(colorPairID));
                    }
                }
            }
            amplitude_idx_offset += polygon.size() * points_per_vertex_side;
        }
    }
}

struct Particle {
    float x, y;
    float vx, vy;
    float life;
    float initial_amplitude;
};

void drawGalaxy(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    static std::vector<Particle> particles;
    static std::mt19937 generator(std::chrono::system_clock::now().time_since_epoch().count());
    static std::uniform_real_distribution<float> dis_angle(0.0f, 2.0f * 3.1415926535f);
    static std::uniform_real_distribution<float> dis_velocity(0.5f, 1.5f);
    static std::uniform_real_distribution<float> dis_life(0.5f, 1.5f);
    const int max_particles = 1000;
    const float particle_spawn_rate_factor = 0.05f;
    const float particle_life_decay_rate = 0.03f;
    const float base_spawn_y = height * 0.9f;
    const float base_spawn_x = width / 2.0f;
    float sum_sq = 0;
    for (int i = 0; i < BUFFER_FRAMES; ++i) {
        float mono_sample = (static_cast<float>(leftData[i]) + static_cast<float>(rightData[i])) / 2.0f;
        sum_sq += mono_sample * mono_sample;
    }
    float overall_amplitude = (BUFFER_FRAMES > 0) ? sqrtf(sum_sq / BUFFER_FRAMES) / 32767.0f : 0.0f;
    overall_amplitude = std::max(0.0f, std::min(1.0f, overall_amplitude));
    if (audio_active && overall_amplitude > 0.05f) {
        int particles_to_spawn = static_cast<int>(max_particles * particle_spawn_rate_factor * overall_amplitude);
        for (int i = 0; i < particles_to_spawn && particles.size() < max_particles; ++i) {
            Particle p;
            p.x = base_spawn_x + (dis_angle(generator) - 1.0f) * 5.0f;
            p.y = base_spawn_y;
            float angle = dis_angle(generator);
            float speed = dis_velocity(generator) * overall_amplitude * 2.0f;
            p.vx = cosf(angle) * speed * 0.4f;
            p.vy = -std::abs(sinf(angle)) * speed * 2.5f;
            p.life = dis_life(generator);
            p.initial_amplitude = overall_amplitude;
            particles.push_back(p);
        }
    }
    std::vector<Particle> active_particles;
    for (auto& p : particles) {
        p.x += p.vx;
        p.y += p.vy;
        p.vy += 0.05f;
        p.life -= particle_life_decay_rate;
        if (p.life > 0.0f && p.x >= 0 && p.x < width && p.y >= 0 && p.y < height) {
            active_particles.push_back(p);
            float display_amplitude = p.initial_amplitude * (p.life / dis_life.b());
            int colorPairID = selectColorByAmplitude(display_amplitude, colorPairIDs);
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, static_cast<int>(p.y), static_cast<int>(p.x), ACS_DIAMOND);
            wattroff(win, COLOR_PAIR(colorPairID));
        }
    }
    particles = active_particles;
}

void drawOscilloscope(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, int edgePairID) {
    int channelHeight = height / 2;
    int rightChannelOffset = channelHeight;
    for (int x = 0; x < width; x++) {
        float sample_pos = static_cast<float>(x) / (width - 1) * (BUFFER_FRAMES - 1);
        int sample_idx1 = static_cast<int>(sample_pos);
        int sample_idx2 = std::min(sample_idx1 + 1, BUFFER_FRAMES - 1);
        float blend = sample_pos - sample_idx1;
        int16_t left_sample = static_cast<int16_t>(leftData[sample_idx1] * (1.0f - blend) + leftData[sample_idx2] * blend);
        int16_t right_sample = static_cast<int16_t>(rightData[sample_idx1] * (1.0f - blend) + rightData[sample_idx2] * blend);
        float range = 65536.0f;
        int y_left = channelHeight / 2 - static_cast<int>((static_cast<float>(left_sample) / range) * channelHeight);
        int y_right = channelHeight / 2 - static_cast<int>((static_cast<float>(right_sample) / range) * channelHeight);
        y_left = std::max(0, std::min(channelHeight - 1, y_left));
        y_right = std::max(0, std::min(channelHeight - 1, y_right));
        float left_amplitude = static_cast<float>(std::abs(left_sample)) / 32767.0f;
        float right_amplitude = static_cast<float>(std::abs(right_sample)) / 32767.0f;
        int pairID_l = selectColorByAmplitude(left_amplitude, colorPairIDs);
        wattron(win, COLOR_PAIR(pairID_l));
        mvwaddch(win, y_left, x, ACS_VLINE);
        wattroff(win, COLOR_PAIR(pairID_l));
        int pairID_r = selectColorByAmplitude(right_amplitude, colorPairIDs);
        wattron(win, COLOR_PAIR(pairID_r));
        mvwaddch(win, y_right + rightChannelOffset, x, ACS_VLINE);
        wattroff(win, COLOR_PAIR(pairID_r));
    }
}

void drawVuMeter(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    static float leftLevel = 0.0f, rightLevel = 0.0f;
    static float leftColorDecay = 0.0f, rightColorDecay = 0.0f;
    const float color_decay_rate = 0.025f;
    if (!audio_active) {
        leftLevel = 0.0f; rightLevel = 0.0f;
        leftColorDecay = 0.0f; rightColorDecay = 0.0f;
    }
    float left_current_level = 0.0f, right_current_level = 0.0f;
    if (vuMeterMode == VU_PEAK) {
        int16_t left_peak = 0, right_peak = 0;
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            if (std::abs(leftData[i]) > left_peak) left_peak = std::abs(leftData[i]);
            if (std::abs(rightData[i]) > right_peak) right_peak = std::abs(rightData[i]);
        }
        left_current_level = static_cast<float>(left_peak) / 32767.0f;
        right_current_level = static_cast<float>(right_peak) / 32767.0f;
    } else {
        float left_sum_sq = 0.0f, right_sum_sq = 0.0f;
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            left_sum_sq += static_cast<float>(leftData[i]) * leftData[i];
            right_sum_sq += static_cast<float>(rightData[i]) * rightData[i];
        }
        left_current_level = sqrtf(left_sum_sq / BUFFER_FRAMES) / 32767.0f;
        right_current_level = sqrtf(right_sum_sq / BUFFER_FRAMES) / 32767.0f;
    }
    const float rise_factor = 0.6f;
    if (left_current_level > leftLevel) leftLevel += (left_current_level - leftLevel) * rise_factor;
    else leftLevel = std::max(0.0f, leftLevel - decay__factor);
    if (right_current_level > rightLevel) rightLevel += (right_current_level - rightLevel) * rise_factor;
    else rightLevel = std::max(0.0f, rightLevel - decay__factor);
    int leftPairID = getFadedColorPairID(leftLevel, leftColorDecay, colorPairIDs, color_decay_rate);
    int rightPairID = getFadedColorPairID(rightLevel, rightColorDecay, colorPairIDs, color_decay_rate);
    int channelHeight = height / 2;
    int left_bar_width = std::min(width, static_cast<int>(leftLevel * width));
    int right_bar_width = std::min(width, static_cast<int>(rightLevel * width));
    if (left_bar_width > 0) {
        wattron(win, COLOR_PAIR(leftPairID));
        for (int y = 0; y < channelHeight; y++) {
            for (int x = 0; x < left_bar_width; x++) {
                mvwaddch(win, y, x, ACS_BLOCK);
            }
        }
        wattroff(win, COLOR_PAIR(leftPairID));
    }
    if (right_bar_width > 0) {
        wattron(win, COLOR_PAIR(rightPairID));
        for (int y = 0; y < channelHeight; y++) {
            for (int x = width - right_bar_width; x < width; x++) {
                mvwaddch(win, y + channelHeight, x, ACS_BLOCK);
            }
        }
        wattroff(win, COLOR_PAIR(rightPairID));
    }
}

void drawBarGraph(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    const int num_bars = 32;
    const int spacing = 1;
    static std::vector<float> leftPeakHeights(num_bars, 0.0f), rightPeakHeights(num_bars, 0.0f);
    static std::vector<float> leftColorDecay(num_bars, 0.0f), rightColorDecay(num_bars, 0.0f);
    const float color_decay_rate = 0.025f;
    if (!audio_active) {
        std::fill(leftPeakHeights.begin(), leftPeakHeights.end(), 0.0f);
        std::fill(rightPeakHeights.begin(), rightPeakHeights.end(), 0.0f);
        std::fill(leftColorDecay.begin(), leftColorDecay.end(), 0.0f);
        std::fill(rightColorDecay.begin(), rightColorDecay.end(), 0.0f);
    }
    int total_bar_width = std::max(0, width - (spacing * (num_bars - 1)));
    int base_bar_width = total_bar_width / num_bars;
    int remainder = total_bar_width % num_bars;
    int channelHeight = height / 2;
    auto process_channel = [&](const int16_t* data, std::vector<float>& peakHeights, std::vector<float>& colorDecay, int y_offset, bool is_top_channel) {
        int current_x = 0;
        for (int bar = 0; bar < num_bars; bar++) {
            int start_idx = bar * BUFFER_FRAMES / num_bars;
            int end_idx = (bar + 1) * BUFFER_FRAMES / num_bars;
            float sum_sq = 0;
            int count = 0;
            for (int i = start_idx; i < end_idx && i < BUFFER_FRAMES; i++) {
                float sample = data[i] / 32768.0f;
                sum_sq += sample * sample;
                count++;
            }
            float rms = (count > 0) ? sqrtf(sum_sq / count) : 0.0f;
            const float rise_factor = 0.6f;
            if (rms > peakHeights[bar]) peakHeights[bar] += (rms - peakHeights[bar]) * rise_factor;
            else peakHeights[bar] = std::max(0.0f, peakHeights[bar] - decay__factor);
            int pairID = getFadedColorPairID(peakHeights[bar], colorDecay[bar], colorPairIDs, color_decay_rate);
            int bar_height = std::min(channelHeight, static_cast<int>(peakHeights[bar] * channelHeight * 1.5f));
            int bar_width = base_bar_width + (bar < remainder ? 1 : 0);
            if (bar_height > 0 && bar_width > 0 && current_x < width) {
                bar_width = std::min(bar_width, width - current_x);
                wattron(win, COLOR_PAIR(pairID));
                for (int col = 0; col < bar_width; col++) {
                    for (int y = 0; y < bar_height; y++) {
                        int y_pos = is_top_channel ? y_offset + channelHeight - 1 - y : y_offset + y;
                        mvwaddch(win, y_pos, current_x + col, ACS_BLOCK);
                    }
                }
                wattroff(win, COLOR_PAIR(pairID));
            }
            current_x += bar_width + (bar < num_bars - 1 ? spacing : 0);
        }
    };
    process_channel(leftData, leftPeakHeights, leftColorDecay, 0, true);
    process_channel(rightData, rightPeakHeights, rightColorDecay, channelHeight, false);
}

void drawEllipse(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs) {
    int centerX = width / 2;
    int centerY = height / 2;
    const float PI = 3.1415926535f;
    float max_x_radius = (width / 2.0f) - 1;
    float max_y_radius = (height / 2.0f) - 1;
    for (int i = 0; i < BUFFER_FRAMES; ++i) {
        float mono_sample = (static_cast<float>(leftData[i]) + static_cast<float>(rightData[i])) / 2.0f;
        float radius = std::abs(mono_sample) / 32767.0f;
        float angle = (2.0f * PI * i) / BUFFER_FRAMES;
        float y = radius * max_y_radius * sinf(angle) * 0.7f;
        float x = radius * max_x_radius * cosf(angle);
        int colorPairID = selectColorByAmplitude(radius, colorPairIDs);
        int screen_x = centerX + static_cast<int>(x);
        int screen_y = centerY + static_cast<int>(y);
        if (screen_x >= 0 && screen_x < width && screen_y >= 0 && screen_y < height) {
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, screen_y, screen_x, '.');
            wattroff(win, COLOR_PAIR(colorPairID));
        }
    }
}

void drawEclipse(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs) {
    const int num_points = 128;
    static std::vector<float> pointAmplitudes(num_points, 0.0f);
    const float rise_factor = 0.5f;
    const float PI = 3.1415926535f;
    int centerX = width / 2;
    int centerY = height / 2;
    float max_radius = std::min(width / 3.0f, height / 2.0f);
    for (int i = 0; i < num_points; ++i) {
        size_t start_idx = i * BUFFER_FRAMES / num_points;
        size_t end_idx = (i + 1) * BUFFER_FRAMES / num_points;
        float sum_sq = 0;
        int count = 0;
        for (size_t j = start_idx; j < end_idx; ++j) {
            float mono_sample = (static_cast<float>(leftData[j]) + static_cast<float>(rightData[j])) / 2.0f;
            sum_sq += mono_sample * mono_sample;
            count++;
        }
        float current_rms = (count > 0) ? sqrtf(sum_sq / count) / 32767.0f : 0.0f;
        if (current_rms > pointAmplitudes[i]) pointAmplitudes[i] += (current_rms - pointAmplitudes[i]) * rise_factor;
        else pointAmplitudes[i] = std::max(0.0f, pointAmplitudes[i] - decay__factor);
    }
    const int smoothing_passes = 2;
    std::vector<float> tempAmplitudes(num_points);
    std::vector<float>* readBuffer = &pointAmplitudes;
    std::vector<float>* writeBuffer = &tempAmplitudes;
    for (int pass = 0; pass < smoothing_passes; ++pass) {
        for (int i = 0; i < num_points; ++i) {
            int prev_idx = (i == 0) ? num_points - 1 : i - 1;
            int next_idx = (i == num_points - 1) ? 0 : i + 1;
            (*writeBuffer)[i] = ((*readBuffer)[prev_idx] * 0.25f) + ((*readBuffer)[i] * 0.5f) + ((*readBuffer)[next_idx] * 0.25f);
        }
        std::swap(readBuffer, writeBuffer);
    }
    if (readBuffer != &pointAmplitudes) pointAmplitudes = *readBuffer;
    for (int i = 0; i < num_points; ++i) {
        float angle = 2.0f * PI * i / num_points;
        float amplitude = pointAmplitudes[i];
        float radius = max_radius * (0.4f + amplitude * 1.5f);
        float inner_radius = max_radius * (0.2f + amplitude * 0.5f);
        int x = static_cast<int>(centerX + cosf(angle) * radius);
        int y = static_cast<int>(centerY + sinf(angle) * radius * 0.6);
        int inner_x = static_cast<int>(centerX + cosf(angle) * inner_radius);
        int inner_y = static_cast<int>(centerY + sinf(angle) * inner_radius * 0.6);
        int colorPairID = selectColorByAmplitude(amplitude, colorPairIDs);
        if (x >= 0 && x < width && y >= 0 && y < height) {
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, y, x, ACS_DIAMOND);
            wattroff(win, COLOR_PAIR(colorPairID));
        }
        if (inner_x >= 0 && inner_x < width && inner_y >= 0 && inner_y < height) {
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, inner_y, inner_x, '.');
            wattroff(win, COLOR_PAIR(colorPairID));
        }
    }
}

void toggleVuMeterMode(bool upArrow) {
    vuMeterMode = upArrow ? VU_PEAK : VU_RMS;
}

const char* getVuMeterModeName() {
    return (vuMeterMode == VU_PEAK) ? "PEAK" : "RMS";
}
