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
    // Clamp the amplitude between 0.0 and 1.0
    amplitude_percent = std::max(0.0f, std::min(1.0f, amplitude_percent));

    // Calculate the color index.
    // We multiply by (size - 1) to get an index within the vector's bounds.
    int colorIdx = static_cast<int>(amplitude_percent * (colorPairIDs.size() - 1));

    return colorPairIDs[colorIdx];
}

// Global decay rate for peak meters
const float decay_rate = 0.025f;

// Visualization functions
void drawOscilloscope(int width, int height,  const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, int edgePairID) {
    int samples_per_col = BUFFER_FRAMES / width;
    if (samples_per_col < 1) samples_per_col = 1;

    int edge_width = std::min(5, width / 20);
    int channelHeight = height / 2; // Split screen
    int rightChannelOffset = channelHeight;

    // Left channel (top)
    for (int x = 0; x < width; x++) {
        int16_t min_sample = 32767;
        int16_t max_sample = -32768;

        int start_idx = x * samples_per_col;
        int end_idx = start_idx + samples_per_col;
        for (int i = start_idx; i < end_idx && i < BUFFER_FRAMES; i++) {
            if (leftData[i] < min_sample) min_sample = leftData[i];
            if (leftData[i] > max_sample) max_sample = leftData[i];
        }

        float range = 65536.0f;
        int top = channelHeight / 2 - static_cast<int>((static_cast<float>(max_sample) / range) * channelHeight);
        int bottom = channelHeight / 2 - static_cast<int>((static_cast<float>(min_sample) / range) * channelHeight);

        top = std::max(0, std::min(top, channelHeight - 1));
        bottom = std::max(0, std::min(bottom, channelHeight - 1));

        int pairID;
        if (x < edge_width || x >= width - edge_width) {
            pairID = edgePairID;
        } else {
            int16_t true_peak = std::max(std::abs(min_sample), std::abs(max_sample));
            float amplitude_percent = static_cast<float>(true_peak) / 32767.0f;
            pairID = selectColorByAmplitude(amplitude_percent, colorPairIDs);
        }

        attron(COLOR_PAIR(pairID));
        for (int y = top; y <= bottom; y++) {
            mvaddch(y, x, ACS_VLINE);
        }
        attroff(COLOR_PAIR(pairID));
    }

    // Right channel (bottom)
    for (int x = 0; x < width; x++) {
        int16_t min_sample = 32767;
        int16_t max_sample = -32768;

        int start_idx = x * samples_per_col;
        int end_idx = start_idx + samples_per_col;
        for (int i = start_idx; i < end_idx && i < BUFFER_FRAMES; i++) {
            if (rightData[i] < min_sample) min_sample = leftData[i];
            if (rightData[i] > max_sample) max_sample = leftData[i];
        }

        float range = 65536.0f;
        int top = channelHeight / 2 - static_cast<int>((static_cast<float>(max_sample) / range) * channelHeight);
        int bottom = channelHeight / 2 - static_cast<int>((static_cast<float>(min_sample) / range) * channelHeight);

        top = std::max(0, std::min(top, channelHeight - 1));
        bottom = std::max(0, std::min(bottom, channelHeight - 1));

        int pairID;
        if (x < edge_width || x >= width - edge_width) {
            pairID = edgePairID;
        } else {
            int16_t true_peak = std::max(std::abs(min_sample), std::abs(max_sample));
            float amplitude_percent = static_cast<float>(true_peak) / 32767.0f;
            pairID = selectColorByAmplitude(amplitude_percent, colorPairIDs);
        }

        attron(COLOR_PAIR(pairID));
        for (int y = top; y <= bottom; y++) {
            mvaddch(y + rightChannelOffset, x, ACS_VLINE);
        }
        attroff(COLOR_PAIR(pairID));
    }
}

void drawVuMeter(int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs)
{
    static float leftPeak = 0.0f;
    static float rightPeak = 0.0f;

    // Calculate new peak amplitudes from current buffer
    int16_t new_left_peak = 0;
    int16_t new_right_peak = 0;
    for (int i = 0; i < BUFFER_FRAMES; i++)
    {
        int16_t left_abs = std::abs(leftData[i]);
        int16_t right_abs = std::abs(rightData[i]);
        if (left_abs > new_left_peak) new_left_peak = left_abs;
        if (right_abs > new_right_peak) new_right_peak = right_abs;
    }

    // Normalize new peaks to 0.0-1.0 range
    float new_left_normalized = static_cast<float>(new_left_peak) / 32767.0f;
    float new_right_normalized = static_cast<float>(new_right_peak) / 32767.0f;

    // Apply decay to peaks
    if (new_left_normalized > leftPeak) {
        leftPeak = new_left_normalized;
    } else {
        leftPeak -= decay_rate;
        if (leftPeak < 0.0f) leftPeak = 0.0f;
    }

    if (new_right_normalized > rightPeak) {
        rightPeak = new_right_normalized;
    } else {
        rightPeak -= decay_rate;
        if (rightPeak < 0.0f) rightPeak = 0.0f;
    }

    int channelHeight = height / 2;
    int rightChannelOffset = channelHeight;

    // Draw left VU meter (top)
    int left_bar_width = static_cast<int>(leftPeak * width);
    left_bar_width = std::min(left_bar_width, width);
    int leftPairID = selectColorByAmplitude(leftPeak, colorPairIDs);

    // Draw bar (left to right)
    attron(COLOR_PAIR(leftPairID));
    for (int y = 0; y < channelHeight; y++)
    {
        for (int x = 0; x < left_bar_width; x++)
        {
            mvaddch(y, x, ' ');
        }
    }
    attroff(COLOR_PAIR(leftPairID));

    // Draw peak indicator
    if (left_bar_width > 0)
    {
        attron(A_REVERSE | COLOR_PAIR(leftPairID));
        for (int y = 0; y < channelHeight; y++)
        {
            mvaddch(y, left_bar_width - 1, ' ');
        }
        attroff(A_REVERSE | COLOR_PAIR(leftPairID));
    }

    // Draw right VU meter (bottom)
    int right_bar_width = static_cast<int>(rightPeak * width);
    right_bar_width = std::min(right_bar_width, width);
    int rightPairID = selectColorByAmplitude(rightPeak, colorPairIDs);

    // Draw bar (right to left)
    attron(COLOR_PAIR(rightPairID));
    for (int y = 0; y < channelHeight; y++) {
        for (int x = width - 1; x >= width - right_bar_width; x--) {
            mvaddch(y + rightChannelOffset, x, ' ');
        }
    }
    attroff(COLOR_PAIR(rightPairID));

    // Draw peak indicator
    if (right_bar_width > 0) {
        attron(A_REVERSE | COLOR_PAIR(rightPairID));
        for (int y = 0; y < channelHeight; y++) {
            mvaddch(y + rightChannelOffset, width - right_bar_width, ' ');
        }
        attroff(A_REVERSE | COLOR_PAIR(rightPairID));
    }
}
void drawBarGraph(int width, int height,const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs)
{
    const int num_bars = 32;
    static std::vector<float> leftPeakHeights(num_bars, 0.0f);
    static std::vector<float> rightPeakHeights(num_bars, 0.0f);
    const float decay_rate_local = 0.025f;

    int bar_width = std::max(1, (width - num_bars + 1) / num_bars);
    int spacing = 1;
    int channelHeight = height / 2;
    int rightChannelOffset = channelHeight;

    // --- Left channel (top) ---
    for (int bar = 0; bar < num_bars; bar++) {
        int start_idx = bar * BUFFER_FRAMES / num_bars;
        int end_idx = (bar + 1) * BUFFER_FRAMES / num_bars;
        float sum_sq = 0;
        int count = 0;
        for (int i = start_idx; i < end_idx; i++) {
            float sample = leftData[i] / 32768.0f;
            sum_sq += sample * sample;
            count++;
        }
        float rms = sqrtf(sum_sq / count);

        if (rms > leftPeakHeights[bar]) {
            leftPeakHeights[bar] = rms;
        } else {
            leftPeakHeights[bar] -= decay_rate_local;
            if (leftPeakHeights[bar] < 0) {
                leftPeakHeights[bar] = 0;
            }
        }

        int bar_height = static_cast<int>(leftPeakHeights[bar] * channelHeight * 1.5f);
        bar_height = std::min(bar_height, channelHeight);
        int x = bar * (bar_width + spacing);

        // Choose color based on the current peak height
        int pairID = selectColorByAmplitude(leftPeakHeights[bar], colorPairIDs);

        attron(COLOR_PAIR(pairID));
        for (int col = 0; col < bar_width; col++) {
            if (x + col >= width) break;
            for (int y = channelHeight - 1; y >= channelHeight - bar_height; y--) {
                mvaddch(y, x + col, ' ');
            }
        }
        attroff(COLOR_PAIR(pairID));
    }

    // --- Right channel (bottom) ---
    for (int bar = 0; bar < num_bars; bar++) {
        int start_idx = bar * BUFFER_FRAMES / num_bars;
        int end_idx = (bar + 1) * BUFFER_FRAMES / num_bars;
        float sum_sq = 0;
        int count = 0;
        for (int i = start_idx; i < end_idx; i++) {
            float sample = rightData[i] / 32768.0f;
            sum_sq += sample * sample;
            count++;
        }
        float rms = sqrtf(sum_sq / count);

        if (rms > rightPeakHeights[bar]) {
            rightPeakHeights[bar] = rms;
        } else {
            rightPeakHeights[bar] -= decay_rate_local;
            if (rightPeakHeights[bar] < 0) {
                rightPeakHeights[bar] = 0;
            }
        }

        int bar_height = static_cast<int>(rightPeakHeights[bar] * channelHeight * 1.5f);
        bar_height = std::min(bar_height, channelHeight);
        int x = bar * (bar_width + spacing);

        // Choose color based on the current peak height
        int pairID = selectColorByAmplitude(rightPeakHeights[bar], colorPairIDs);

        attron(COLOR_PAIR(pairID));
        for (int col = 0; col < bar_width; col++) {
            if (x + col >= width) break;
            for (int y = 0; y < bar_height; y++) {
                mvaddch(y + rightChannelOffset, x + col, ' ');
            }
        }
        attroff(COLOR_PAIR(pairID));
    }
}
