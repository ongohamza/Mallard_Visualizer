#include <ncurses.h>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include "config_parser.h"

// Helper function to select a color from the gradient based on amplitude
int selectColorByAmplitude(float amplitude_percent, const std::vector<int>& colorPairIDs) {
    if (colorPairIDs.empty()) {
        return 1; // Default color if none are configured
    }
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

void drawOscilloscope(int width, int height,  const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, int edgePairID) {
    int samples_per_col = std::max(1, BUFFER_FRAMES / width);
    int channelHeight = height / 2;
    int rightChannelOffset = channelHeight;

    for (int x = 0; x < width; x++) {
        int16_t min_l = 32767, max_l = -32768;
        int16_t min_r = 32767, max_r = -32768;
        int start_idx = x * samples_per_col, end_idx = start_idx + samples_per_col;
        for (int i = start_idx; i < end_idx && i < BUFFER_FRAMES; i++) {
            if (leftData[i] < min_l) min_l = leftData[i];
            if (leftData[i] > max_l) max_l = leftData[i];
            if (rightData[i] < min_r) min_r = rightData[i];
            if (rightData[i] > max_r) max_r = rightData[i];
        }

        float range = 65536.0f;
        int top_l = std::max(0, std::min(channelHeight - 1, channelHeight / 2 - static_cast<int>((static_cast<float>(max_l) / range) * channelHeight)));
        int bot_l = std::max(0, std::min(channelHeight - 1, channelHeight / 2 - static_cast<int>((static_cast<float>(min_l) / range) * channelHeight)));
        int top_r = std::max(0, std::min(channelHeight - 1, channelHeight / 2 - static_cast<int>((static_cast<float>(max_r) / range) * channelHeight)));
        int bot_r = std::max(0, std::min(channelHeight - 1, channelHeight / 2 - static_cast<int>((static_cast<float>(min_r) / range) * channelHeight)));

        int pairID_l = selectColorByAmplitude(static_cast<float>(std::max(std::abs(min_l), std::abs(max_l))) / 32767.0f, colorPairIDs);
        attron(COLOR_PAIR(pairID_l));
        for (int y = top_l; y <= bot_l; y++) mvaddch(y, x, ACS_VLINE);
        attroff(COLOR_PAIR(pairID_l));

        int pairID_r = selectColorByAmplitude(static_cast<float>(std::max(std::abs(min_r), std::abs(max_r))) / 32767.0f, colorPairIDs);
        attron(COLOR_PAIR(pairID_r));
        for (int y = top_r; y <= bot_r; y++) mvaddch(y + rightChannelOffset, x, ACS_VLINE);
        attroff(COLOR_PAIR(pairID_r));
    }
}

void drawVuMeter(int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs) {
    static float leftColorDecay = 0.0f, rightColorDecay = 0.0f;
    const float color_decay_rate = 0.025f;
    int16_t left_peak = 0, right_peak = 0;
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        if (std::abs(leftData[i]) > left_peak) left_peak = std::abs(leftData[i]);
        if (std::abs(rightData[i]) > right_peak) right_peak = std::abs(rightData[i]);
    }
    float left_norm = static_cast<float>(left_peak) / 32767.0f, right_norm = static_cast<float>(right_peak) / 32767.0f;
    int leftPairID = getFadedColorPairID(left_norm, leftColorDecay, colorPairIDs, color_decay_rate);
    int rightPairID = getFadedColorPairID(right_norm, rightColorDecay, colorPairIDs, color_decay_rate);
    int channelHeight = height / 2;
    int left_bar_width = std::min(width, static_cast<int>(left_norm * width));
    int right_bar_width = std::min(width, static_cast<int>(right_norm * width));

    attron(COLOR_PAIR(leftPairID));
    for (int y = 0; y < channelHeight; y++) for (int x = 0; x < left_bar_width; x++) mvaddch(y, x, ' ');
    attroff(COLOR_PAIR(leftPairID));
    if (left_bar_width > 0) {
        attron(A_REVERSE | COLOR_PAIR(leftPairID));
        for (int y = 0; y < channelHeight; y++) mvaddch(y, left_bar_width - 1, ' ');
        attroff(A_REVERSE | COLOR_PAIR(leftPairID));
    }

    attron(COLOR_PAIR(rightPairID));
    for (int y = 0; y < channelHeight; y++) for (int x = width - 1; x >= width - right_bar_width; x--) mvaddch(y + channelHeight, x, ' ');
    attroff(COLOR_PAIR(rightPairID));
    if (right_bar_width > 0) {
        attron(A_REVERSE | COLOR_PAIR(rightPairID));
        for (int y = 0; y < channelHeight; y++) mvaddch(y + channelHeight, width - right_bar_width, ' ');
        attroff(A_REVERSE | COLOR_PAIR(rightPairID));
    }
}

void drawBarGraph(int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active)
{
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
    int rightChannelOffset = channelHeight;
    int current_x = 0;

    auto process_channel = [&](const int16_t* data, std::vector<float>& peakHeights, std::vector<float>& colorDecay, int y_offset, bool is_top_channel) {
        current_x = 0;
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
            if (rms > peakHeights[bar]) {
                peakHeights[bar] += (rms - peakHeights[bar]) * rise_factor;
            } else {
                peakHeights[bar] = std::max(0.0f, peakHeights[bar] - decay__factor);
            }

            int pairID = getFadedColorPairID(peakHeights[bar], colorDecay[bar], colorPairIDs, color_decay_rate);
            int bar_height = std::min(channelHeight, static_cast<int>(peakHeights[bar] * channelHeight * 1.5f));
            int bar_width = base_bar_width + (bar < remainder ? 1 : 0);

            if (bar_height > 0 && bar_width > 0 && current_x < width) {
                bar_width = std::min(bar_width, width - current_x);
                attron(COLOR_PAIR(pairID));
                for (int col = 0; col < bar_width; col++) {
                    for (int y = 0; y < bar_height; y++) {
                        int y_pos = is_top_channel ? y_offset + channelHeight - 1 - y : y_offset + y;
                        // **THE FIX**: Draw a solid block character using the foreground color
                        // instead of a space with a background color.
                        mvaddch(y_pos, current_x + col, ACS_BLOCK);
                    }
                }
                attroff(COLOR_PAIR(pairID));
            }
            current_x += bar_width + (bar < num_bars - 1 ? spacing : 0);
        }
    };

    process_channel(leftData, leftPeakHeights, leftColorDecay, 0, true);
    process_channel(rightData, rightPeakHeights, rightColorDecay, rightChannelOffset, false);
}
