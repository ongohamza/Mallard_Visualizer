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

// VU Meter modes
enum VuMeterMode {
    VU_RMS,
    VU_PEAK
};

// Global VU meter mode (default to RMS)
VuMeterMode vuMeterMode = VU_RMS;

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
    int channelHeight = height / 2;
    int rightChannelOffset = channelHeight;

    // Fix: Handle widths larger than BUFFER_FRAMES by stretching the waveform
    for (int x = 0; x < width; x++) {
        // Map x position to sample index with interpolation for stretching
        float sample_pos = static_cast<float>(x) / (width - 1) * (BUFFER_FRAMES - 1);
        int sample_idx1 = static_cast<int>(sample_pos);
        int sample_idx2 = std::min(sample_idx1 + 1, BUFFER_FRAMES - 1);
        float blend = sample_pos - sample_idx1;
        
        // Interpolate between samples for smooth stretching
        int16_t left_sample = static_cast<int16_t>(
            leftData[sample_idx1] * (1.0f - blend) + leftData[sample_idx2] * blend
        );
        int16_t right_sample = static_cast<int16_t>(
            rightData[sample_idx1] * (1.0f - blend) + rightData[sample_idx2] * blend
        );

        // Calculate y positions for left and right channels
        float range = 65536.0f;
        int y_left = channelHeight / 2 - static_cast<int>((static_cast<float>(left_sample) / range) * channelHeight);
        int y_right = channelHeight / 2 - static_cast<int>((static_cast<float>(right_sample) / range) * channelHeight);
        
        // Clamp y positions to valid range
        y_left = std::max(0, std::min(channelHeight - 1, y_left));
        y_right = std::max(0, std::min(channelHeight - 1, y_right));

        // Calculate amplitude for color selection
        float left_amplitude = static_cast<float>(std::abs(left_sample)) / 32767.0f;
        float right_amplitude = static_cast<float>(std::abs(right_sample)) / 32767.0f;

        // Draw left channel
        int pairID_l = selectColorByAmplitude(left_amplitude, colorPairIDs);
        attron(COLOR_PAIR(pairID_l));
        mvaddch(y_left, x, ACS_VLINE);
        attroff(COLOR_PAIR(pairID_l));

        // Draw right channel  
        int pairID_r = selectColorByAmplitude(right_amplitude, colorPairIDs);
        attron(COLOR_PAIR(pairID_r));
        mvaddch(y_right + rightChannelOffset, x, ACS_VLINE);
        attroff(COLOR_PAIR(pairID_r));
    }
}

void drawVuMeter(int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    static float leftLevel = 0.0f, rightLevel = 0.0f;
    static float leftColorDecay = 0.0f, rightColorDecay = 0.0f;
    const float color_decay_rate = 0.025f;

    if (!audio_active) {
        leftLevel = 0.0f;
        rightLevel = 0.0f;
        leftColorDecay = 0.0f;
        rightColorDecay = 0.0f;
    }

    float left_current_level = 0.0f, right_current_level = 0.0f;
    
    if (vuMeterMode == VU_PEAK) {
        // Peak detection mode
        int16_t left_peak = 0, right_peak = 0;
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            if (std::abs(leftData[i]) > left_peak) left_peak = std::abs(leftData[i]);
            if (std::abs(rightData[i]) > right_peak) right_peak = std::abs(rightData[i]);
        }
        left_current_level = static_cast<float>(left_peak) / 32767.0f;
        right_current_level = static_cast<float>(right_peak) / 32767.0f;
    } else {
        // RMS mode
        float left_sum_sq = 0.0f, right_sum_sq = 0.0f;
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            left_sum_sq += static_cast<float>(leftData[i]) * leftData[i];
            right_sum_sq += static_cast<float>(rightData[i]) * rightData[i];
        }
        left_current_level = sqrtf(left_sum_sq / BUFFER_FRAMES) / 32767.0f;
        right_current_level = sqrtf(right_sum_sq / BUFFER_FRAMES) / 32767.0f;
    }

    // Apply decay to levels
    const float rise_factor = 0.6f;
    
    if (left_current_level > leftLevel) {
        leftLevel += (left_current_level - leftLevel) * rise_factor;
    } else {
        leftLevel = std::max(0.0f, leftLevel - decay__factor);
    }
    
    if (right_current_level > rightLevel) {
        rightLevel += (right_current_level - rightLevel) * rise_factor;
    } else {
        rightLevel = std::max(0.0f, rightLevel - decay__factor);
    }

    // Use the level for both bar width and color intensity
    int leftPairID = getFadedColorPairID(leftLevel, leftColorDecay, colorPairIDs, color_decay_rate);
    int rightPairID = getFadedColorPairID(rightLevel, rightColorDecay, colorPairIDs, color_decay_rate);
    
    int channelHeight = height / 2;
    int left_bar_width = std::min(width, static_cast<int>(leftLevel * width));
    int right_bar_width = std::min(width, static_cast<int>(rightLevel * width));

    // Draw left channel using solid block characters
    if (left_bar_width > 0) {
        attron(COLOR_PAIR(leftPairID));
        for (int y = 0; y < channelHeight; y++) {
            for (int x = 0; x < left_bar_width; x++) {
                mvaddch(y, x, ACS_BLOCK);
            }
        }
        attroff(COLOR_PAIR(leftPairID));
    }

    // Draw right channel using solid block characters
    if (right_bar_width > 0) {
        attron(COLOR_PAIR(rightPairID));
        for (int y = 0; y < channelHeight; y++) {
            for (int x = width - right_bar_width; x < width; x++) {
                mvaddch(y + channelHeight, x, ACS_BLOCK);
            }
        }
        attroff(COLOR_PAIR(rightPairID));
    }
}

void drawBarGraph(int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
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

// Function to toggle VU meter mode (called from oscilloscope.cpp)
void toggleVuMeterMode(bool upArrow) {
    if (upArrow) {
        vuMeterMode = VU_PEAK;
    } else {
        vuMeterMode = VU_RMS;
    }
}

// Function to get current VU meter mode name
const char* getVuMeterModeName() {
    return (vuMeterMode == VU_PEAK) ? "PEAK" : "RMS";
}
