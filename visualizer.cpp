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
    VU_RMS,    // Average volume (Root Mean Square)
    VU_PEAK    // Loudest single sample
};
VuMeterMode vuMeterMode = VU_RMS; // Default to RMS

/**
 * @brief Selects a color pair ID from the gradient list based on amplitude.
 * @param amplitude_percent A normalized amplitude value (0.0f to 1.0f).
 * @param colorPairIDs The vector of configured gradient color pair IDs.
 * @return An ncurses color pair ID.
 */
int selectColorByAmplitude(float amplitude_percent, const std::vector<int>& colorPairIDs) {
    if (colorPairIDs.empty()) return 1; // Fallback
    // Clamp the amplitude to the valid range [0.0, 1.0]
    amplitude_percent = std::max(0.0f, std::min(1.0f, amplitude_percent));
    // Map the 0.0-1.0 range to an index in the color vector
    int colorIdx = static_cast<int>(amplitude_percent * (colorPairIDs.size() - 1));
    return colorPairIDs[colorIdx];
}

/**
 * @brief Manages smooth color transitions with decay.
 *
 * This prevents the color from flickering wildly with the audio.
 * It smoothly rises to new peaks and slowly fades back down.
 *
 * @param currentAmplitude The current, "true" amplitude (0.0-1.0).
 * @param lastDecay A reference to the "display" amplitude, which will be updated.
 * @param colorPairIDs The vector of gradient colors.
 * @param decay_rate How fast the color should fade when the audio gets quiet.
 * @return An ncurses color pair ID based on the smoothed `lastDecay` amplitude.
 */
int getFadedColorPairID(float currentAmplitude, float& lastDecay, const std::vector<int>& colorPairIDs, float decay_rate) {
    // If the audio is louder than the last displayed frame, rise to meet it
    if (currentAmplitude > lastDecay) {
        lastDecay = std::min(currentAmplitude, lastDecay + decay_rate);
    } else {
    // If the audio is quieter, slowly decay
        lastDecay = std::max(currentAmplitude, lastDecay - decay_rate);
    }
    // Select the color based on the smoothed amplitude
    return selectColorByAmplitude(lastDecay, colorPairIDs);
}

/**
 * @brief Helper for 'distort' mode. Finds the intersection of a ray and a line segment.
 *
 * This is a 2D line intersection algorithm. It checks if a ray starting from (0,0)
 * in the direction (ray_x, ray_y) intersects the line segment from (p1x, p1y) to (p2x, p2y).
 *
 * @return The distance 't' from the origin to the intersection, or -1.0f if no intersection.
 */
float getIntersectionDist(float ray_x, float ray_y, float p1x, float p1y, float p2x, float p2y) {
    float seg_x = p2x - p1x;
    float seg_y = p2y - p1y;
    // Determinant of the ray and segment vectors
    float det = ray_x * seg_y - ray_y * seg_x;
    // If parallel, no intersection
    if (std::abs(det) < 1e-6) return -1.0f;
    // t = distance along the ray, u = distance along the line segment
    float t = (p1x * seg_y - p1y * seg_x) / det;
    float u = (p1x * ray_y - p1y * ray_x) / det;
    // We only care about intersections:
    // 1. In front of the ray (t > 0)
    // 2. Within the line segment's bounds (0 <= u <= 1)
    if (t > 0 && u >= 0 && u <= 1) return t;
    return -1.0f;
}

/**
 * @brief Draws a custom shape defined in the config file.
 *
 * Supports two types:
 * 1. DISTORT: A ray-casting visualizer. The audio amplitude scales the
 * distance of points from the center along rays.
 * 2. EXPAND: A radial frequency visualizer. The polygon's perimeter is
 * divided by frequency, and the amplitude of each frequency band
 * "pushes" that part of the shape outwards.
 */
void drawCustomShape(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, const CustomVisualizer& visualizer) {
    int centerX = width / 2;
    int centerY = height / 2;

    // --- START OF MODIFICATION for 'distort' ---
    if (visualizer.type == ShapeVisualizerType::DISTORT) {
        if (visualizer.polygons.empty()) return;

        // 1. Use frequency bins and decay, just like 'eclipse'
        const int num_points = 128; // Number of rays/frequency bins
        static std::vector<float> pointAmplitudes(num_points, 0.0f); // Decaying peaks
        const float rise_factor = 0.5f;
        const float PI = 3.1415926535f;
        float scale = std::min(width, height) / 200.0f; // Base scaling factor

        // Calculate RMS for each frequency bin and apply decay
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

        // 2. Apply smoothing passes for a "wavy" effect
        const int smoothing_passes = 2;
        std::vector<float> tempAmplitudes(num_points);
        std::vector<float>* readBuffer = &pointAmplitudes;
        std::vector<float>* writeBuffer = &tempAmplitudes;
        for (int pass = 0; pass < smoothing_passes; ++pass) {
            for (int i = 0; i < num_points; ++i) {
                int prev_idx = (i == 0) ? num_points - 1 : i - 1;
                int next_idx = (i == num_points - 1) ? 0 : i + 1;
                // Weighted average: 25% prev, 50% current, 25% next
                (*writeBuffer)[i] = ((*readBuffer)[prev_idx] * 0.25f) + ((*readBuffer)[i] * 0.5f) + ((*readBuffer)[next_idx] * 0.25f);
            }
            std::swap(readBuffer, writeBuffer); // Swap buffers for next pass
        }
        if (readBuffer != &pointAmplitudes) pointAmplitudes = *readBuffer; // Ensure pointAmplitudes has final data

        // 3. Render the shape using ray-casting
        for (int i = 0; i < num_points; ++i) {
            float amplitude = pointAmplitudes[i]; // Get the smoothed amplitude for this angle
            float angle = 2.0f * PI * i / num_points;
            
            float ray_x = std::cos(angle);
            float ray_y = std::sin(angle);
            float min_dist = -1.0f; // Shortest distance to any polygon edge

            // Check this ray against every line segment in every polygon
            for (const auto& polygon : visualizer.polygons) {
                for (size_t v = 0; v < polygon.size(); ++v) {
                    auto p1 = polygon[v];
                    auto p2 = polygon[(v + 1) % polygon.size()];
                    float dist = getIntersectionDist(ray_x, ray_y, p1.first, p1.second, p2.first, p2.second);
                    // If we found an intersection, see if it's the closest one yet
                    if (dist > 0 && (min_dist < 0 || dist < min_dist)) {
                        min_dist = dist;
                    }
                }
            }

            // If we found an intersection (min_dist > 0)
            if (min_dist > 0) {
                // *** THIS IS THE FIX for the collapsing shape ***
                // We use (1.0f + ...), so when amplitude is 0, it draws the base shape (1.0f).
                // It expands outwards with audio (1.0f + amplitude * 1.5f)
                float final_dist = scale * min_dist * (1.0f + amplitude * 1.5f);
                
                float x = final_dist * ray_x;
                float y = final_dist * ray_y;
                
                int colorPairID = selectColorByAmplitude(amplitude, colorPairIDs);
                int screen_x = centerX + static_cast<int>(x);
                int screen_y = centerY + static_cast<int>(y * 0.6f); // Aspect ratio correction
                if (screen_x >= 0 && screen_x < width && screen_y >= 0 && screen_y < height) {
                    wattron(win, COLOR_PAIR(colorPairID));
                    mvwaddch(win, screen_y, screen_x, '.');
                    wattroff(win, COLOR_PAIR(colorPairID));
                }
            }
        }
    } 
    // --- END OF MODIFICATION ---
    else {
        // --- EXPAND ---
        // This mode maps frequency bins to points on the shape's perimeter.
        size_t total_vertices = 0;
        for (const auto& poly : visualizer.polygons) total_vertices += poly.size();
        if (total_vertices < 2) return;

        // Subdivide the perimeter to get more points for frequencies
        const size_t points_per_vertex_side = 40;
        const size_t total_points = total_vertices * points_per_vertex_side;
        static std::vector<float> pointAmplitudes; // Holds the decaying amplitude for each point
        if(pointAmplitudes.size() != total_points) pointAmplitudes.resize(total_points, 0.0f);

        const float rise_factor = 0.5f; // How fast the points react to new peaks
        for (size_t i = 0; i < total_points; ++i) {
            // Map this point 'i' to a slice (frequency bin) of the audio buffer
            size_t start_idx = i * BUFFER_FRAMES / total_points;
            size_t end_idx = (i + 1) * BUFFER_FRAMES / total_points;
            if(end_idx > BUFFER_FRAMES) end_idx = BUFFER_FRAMES;
            
            // Calculate RMS (average volume) for this frequency bin
            float sum_sq = 0;
            int count = 0;
            for (size_t j = start_idx; j < end_idx; ++j) {
                float mono_sample = (static_cast<float>(leftData[j]) + static_cast<float>(rightData[j])) / 2.0f;
                sum_sq += mono_sample * mono_sample;
                count++;
            }
            float current_rms = (count > 0) ? sqrtf(sum_sq / count) / 32767.0f : 0.0f;
            
            // Apply decay logic (rise fast, fall slow)
            if (current_rms > pointAmplitudes[i]) pointAmplitudes[i] += (current_rms - pointAmplitudes[i]) * rise_factor;
            else pointAmplitudes[i] = std::max(0.0f, pointAmplitudes[i] - decay__factor);
        }

        float scale = std::min(width, height) / 250.0f; // Base scaling factor
        size_t amplitude_idx_offset = 0;
        
        // Iterate through all polygons and their sides
        for (const auto& polygon : visualizer.polygons) {
            if (polygon.size() < 2) continue;
            for (size_t side = 0; side < polygon.size(); ++side) {
                auto p1 = polygon[side]; // Start vertex of the side
                auto p2 = polygon[(side + 1) % polygon.size()]; // End vertex

                // Draw all the points along this side
                for (size_t i = 0; i < points_per_vertex_side; ++i) {
                    // 't' is the position between p1 and p2 (0.0 to 1.0)
                    float t = static_cast<float>(i) / points_per_vertex_side;
                    // Find the base (un-expanded) point via linear interpolation
                    float base_x = p1.first + t * (p2.first - p1.first);
                    float base_y = p1.second + t * (p2.second - p1.second);
                    
                    // Get the amplitude for this specific point
                    size_t amplitude_index = amplitude_idx_offset + side * points_per_vertex_side + i;
                    if(amplitude_index >= total_points) continue;
                    float amplitude = pointAmplitudes[amplitude_index];
                    
                    // Scale the base point outwards by its amplitude
                    // (1.0f + ...) ensures it defaults to 1.0f size when amplitude is 0.
                    float x = base_x * scale * (1.0f + amplitude * 0.5f);
                    float y = base_y * scale * (1.0f + amplitude * 0.5f);
                    
                    int colorPairID = selectColorByAmplitude(amplitude, colorPairIDs);
                    int screen_x = centerX + static_cast<int>(x);
                    int screen_y = centerY + static_cast<int>(y * 0.6f); // Aspect ratio correction
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

// Struct for a single particle in the 'Galaxy' visualizer
struct Particle {
    float x, y;     // Position
    float vx, vy;   // Velocity
    float life;     // Time remaining
    float initial_amplitude; // Amplitude at spawn (for color)
};

/**
 * @brief Draws a particle fountain effect.
 *
 * Overall audio amplitude spawns particles at the bottom center.
 * Particles fly upwards and outwards, affected by "gravity", and fade over time.
 */
void drawGalaxy(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    static std::vector<Particle> particles;
    // Random number generators for particle properties
    static std::mt19937 generator(std::chrono::system_clock::now().time_since_epoch().count());
    static std::uniform_real_distribution<float> dis_angle(0.0f, 2.0f * 3.1415926535f);
    static std::uniform_real_distribution<float> dis_velocity(0.5f, 1.5f);
    static std::uniform_real_distribution<float> dis_life(0.5f, 1.5f);

    const int max_particles = 1000;
    const float particle_spawn_rate_factor = 0.05f;
    const float particle_life_decay_rate = 0.03f;
    const float base_spawn_y = height * 0.9f;
    const float base_spawn_x = width / 2.0f;

    // Calculate overall RMS amplitude of the current buffer
    float sum_sq = 0;
    for (int i = 0; i < BUFFER_FRAMES; ++i) {
        float mono_sample = (static_cast<float>(leftData[i]) + static_cast<float>(rightData[i])) / 2.0f;
        sum_sq += mono_sample * mono_sample;
    }
    float overall_amplitude = (BUFFER_FRAMES > 0) ? sqrtf(sum_sq / BUFFER_FRAMES) / 32767.0f : 0.0f;
    overall_amplitude = std::max(0.0f, std::min(1.0f, overall_amplitude));

    // Spawn new particles if audio is active and loud enough
    if (audio_active && overall_amplitude > 0.05f) {
        int particles_to_spawn = static_cast<int>(max_particles * particle_spawn_rate_factor * overall_amplitude);
        for (int i = 0; i < particles_to_spawn && particles.size() < max_particles; ++i) {
            Particle p;
            p.x = base_spawn_x + (dis_angle(generator) - 1.0f) * 5.0f; // Spawn near center
            p.y = base_spawn_y;
            float angle = dis_angle(generator);
            float speed = dis_velocity(generator) * overall_amplitude * 2.0f;
            p.vx = cosf(angle) * speed * 0.4f; // Horizontal velocity
            p.vy = -std::abs(sinf(angle)) * speed * 2.5f; // Vertical velocity (always up)
            p.life = dis_life(generator);
            p.initial_amplitude = overall_amplitude;
            particles.push_back(p);
        }
    }

    // Update and draw all existing particles
    std::vector<Particle> active_particles;
    for (auto& p : particles) {
        // Basic physics update
        p.x += p.vx;
        p.y += p.vy;
        p.vy += 0.05f; // Gravity
        p.life -= particle_life_decay_rate; // Age the particle

        // If particle is still alive and on-screen, draw it
        if (p.life > 0.0f && p.x >= 0 && p.x < width && p.y >= 0 && p.y < height) {
            active_particles.push_back(p);
            // Color fades as the particle dies
            float display_amplitude = p.initial_amplitude * (p.life / dis_life.b());
            int colorPairID = selectColorByAmplitude(display_amplitude, colorPairIDs);
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, static_cast<int>(p.y), static_cast<int>(p.x), ACS_DIAMOND);
            wattroff(win, COLOR_PAIR(colorPairID));
        }
    }
    // Replace the old particle list with the new list of active particles
    particles = active_particles;
}

/**
 * @brief Draws a classic two-channel oscilloscope.
 *
 * Maps time (sample index) to the X-axis and amplitude to the Y-axis.
 * The window is split, with the Left channel on top and the Right channel on the bottom.
 */
void drawOscilloscope(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, int edgePairID) {
    int channelHeight = height / 2;
    int rightChannelOffset = channelHeight;

    // Iterate over every column (x-pixel) in the window
    for (int x = 0; x < width; x++) {
        // Find the corresponding sample in the audio buffer.
        // This includes linear interpolation for a smoother look.
        float sample_pos = static_cast<float>(x) / (width - 1) * (BUFFER_FRAMES - 1);
        int sample_idx1 = static_cast<int>(sample_pos);
        int sample_idx2 = std::min(sample_idx1 + 1, BUFFER_FRAMES - 1);
        float blend = sample_pos - sample_idx1;

        // Interpolate to find the exact sample value for this 'x' position
        int16_t left_sample = static_cast<int16_t>(leftData[sample_idx1] * (1.0f - blend) + leftData[sample_idx2] * blend);
        int16_t right_sample = static_cast<int16_t>(rightData[sample_idx1] * (1.0f - blend) + rightData[sample_idx2] * blend);

        float range = 65536.0f; // Full range of int16_t
        // Map the sample value (-32768 to 32767) to a Y-coordinate in the channel
        int y_left = channelHeight / 2 - static_cast<int>((static_cast<float>(left_sample) / range) * channelHeight);
        int y_right = channelHeight / 2 - static_cast<int>((static_cast<float>(right_sample) / range) * channelHeight);
        // Clamp Y-values to be within the channel's bounds
        y_left = std::max(0, std::min(channelHeight - 1, y_left));
        y_right = std::max(0, std::min(channelHeight - 1, y_right));

        // Select color based on the amplitude of this specific sample
        float left_amplitude = static_cast<float>(std::abs(left_sample)) / 32767.0f;
        float right_amplitude = static_cast<float>(std::abs(right_sample)) / 32767.0f;
        
        // Draw the left channel point
        int pairID_l = selectColorByAmplitude(left_amplitude, colorPairIDs);
        wattron(win, COLOR_PAIR(pairID_l));
        mvwaddch(win, y_left, x, ACS_VLINE);
        wattroff(win, COLOR_PAIR(pairID_l));

        // Draw the right channel point (offset by channelHeight)
        int pairID_r = selectColorByAmplitude(right_amplitude, colorPairIDs);
        wattron(win, COLOR_PAIR(pairID_r));
        mvwaddch(win, y_right + rightChannelOffset, x, ACS_VLINE);
        wattroff(win, COLOR_PAIR(pairID_r));
    }
}

/**
 * @brief Draws a horizontal stereo VU (Volume Unit) meter.
 *
 * Can operate in PEAK or RMS mode. Shows Left channel volume on top,
 * Right channel on the bottom. Includes decay for a smooth, readable meter.
 */
void drawVuMeter(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    // 'Level' is the displayed level (with decay), 'ColorDecay' is for smooth color fading
    static float leftLevel = 0.0f, rightLevel = 0.0f;
    static float leftColorDecay = 0.0f, rightColorDecay = 0.0f;
    const float color_decay_rate = 0.025f;

    // Reset levels if audio disconnects
    if (!audio_active) {
        leftLevel = 0.0f; rightLevel = 0.0f;
        leftColorDecay = 0.0f; rightColorDecay = 0.0f;
    }

    float left_current_level = 0.0f, right_current_level = 0.0f;

    // Calculate the "true" level for this buffer based on the mode
    if (vuMeterMode == VU_PEAK) {
        // --- PEAK Mode ---
        // Find the loudest single sample in the buffer
        int16_t left_peak = 0, right_peak = 0;
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            if (std::abs(leftData[i]) > left_peak) left_peak = std::abs(leftData[i]);
            if (std::abs(rightData[i]) > right_peak) right_peak = std::abs(rightData[i]);
        }
        left_current_level = static_cast<float>(left_peak) / 32767.0f;
        right_current_level = static_cast<float>(right_peak) / 32767.0f;
    } else {
        // --- RMS Mode ---
        // Calculate the Root Mean Square (average power) of the buffer
        float left_sum_sq = 0.0f, right_sum_sq = 0.0f;
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            left_sum_sq += static_cast<float>(leftData[i]) * leftData[i];
            right_sum_sq += static_cast<float>(rightData[i]) * rightData[i];
        }
        left_current_level = sqrtf(left_sum_sq / BUFFER_FRAMES) / 32767.0f;
        right_current_level = sqrtf(right_sum_sq / BUFFER_FRAMES) / 32767.0f;
    }

    // Apply decay logic (rise fast, fall slow) to the displayed level
    const float rise_factor = 0.6f;
    if (left_current_level > leftLevel) leftLevel += (left_current_level - leftLevel) * rise_factor;
    else leftLevel = std::max(0.0f, leftLevel - decay__factor); // decay__factor is global
    if (right_current_level > rightLevel) rightLevel += (right_current_level - rightLevel) * rise_factor;
    else rightLevel = std::max(0.0f, rightLevel - decay__factor);

    // Get smoothed colors
    int leftPairID = getFadedColorPairID(leftLevel, leftColorDecay, colorPairIDs, color_decay_rate);
    int rightPairID = getFadedColorPairID(rightLevel, rightColorDecay, colorPairIDs, color_decay_rate);

    int channelHeight = height / 2;
    // Calculate the width of the bar based on the level
    int left_bar_width = std::min(width, static_cast<int>(leftLevel * width));
    int right_bar_width = std::min(width, static_cast<int>(rightLevel * width));

    // Draw the left channel bar (top)
    if (left_bar_width > 0) {
        wattron(win, COLOR_PAIR(leftPairID));
        for (int y = 0; y < channelHeight; y++) {
            for (int x = 0; x < left_bar_width; x++) {
                mvwaddch(win, y, x, ACS_BLOCK);
            }
        }
        wattroff(win, COLOR_PAIR(leftPairID));
    }

    // Draw the right channel bar (bottom, aligned to the right)
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

/**
 * @brief Draws a stereo frequency bar graph (spectrum visualizer).
 *
 * Divides the audio buffer into frequency bins (`num_bars`) and draws
 * the RMS volume of each bin as a vertical bar. Left channel is on top
 * (bars go down), Right channel is on bottom (bars go up).
 */
void drawBarGraph(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs, bool audio_active) {
    const int num_bars = 32; // How many frequency bins
    const int spacing = 1;   // Space between bars
    // 'peakHeights' holds the decaying peak for each bar
    static std::vector<float> leftPeakHeights(num_bars, 0.0f), rightPeakHeights(num_bars, 0.0f);
    // 'colorDecay' is for smooth color fading for each bar
    static std::vector<float> leftColorDecay(num_bars, 0.0f), rightColorDecay(num_bars, 0.0f);
    const float color_decay_rate = 0.025f;

    // Reset peaks if audio disconnects
    if (!audio_active) {
        std::fill(leftPeakHeights.begin(), leftPeakHeights.end(), 0.0f);
        std::fill(rightPeakHeights.begin(), rightPeakHeights.end(), 0.0f);
        std::fill(leftColorDecay.begin(), leftColorDecay.end(), 0.0f);
        std::fill(rightColorDecay.begin(), rightColorDecay.end(), 0.0f);
    }

    // Calculate bar width, distributing remainder pixels
    int total_bar_width = std::max(0, width - (spacing * (num_bars - 1)));
    int base_bar_width = total_bar_width / num_bars;
    int remainder = total_bar_width % num_bars;
    int channelHeight = height / 2;

    // Lambda function to process and draw one channel (L or R)
    auto process_channel = [&](const int16_t* data, std::vector<float>& peakHeights, std::vector<float>& colorDecay, int y_offset, bool is_top_channel) {
        int current_x = 0;
        for (int bar = 0; bar < num_bars; bar++) {
            // Find the slice of the audio buffer for this frequency bin
            int start_idx = bar * BUFFER_FRAMES / num_bars;
            int end_idx = (bar + 1) * BUFFER_FRAMES / num_bars;
            
            // Calculate RMS for this bin
            float sum_sq = 0;
            int count = 0;
            for (int i = start_idx; i < end_idx && i < BUFFER_FRAMES; i++) {
                float sample = data[i] / 32768.0f;
                sum_sq += sample * sample;
                count++;
            }
            float rms = (count > 0) ? sqrtf(sum_sq / count) : 0.0f;
            
            // Apply decay logic to the bar's peak
            const float rise_factor = 0.6f;
            if (rms > peakHeights[bar]) peakHeights[bar] += (rms - peakHeights[bar]) * rise_factor;
            else peakHeights[bar] = std::max(0.0f, peakHeights[bar] - decay__factor);
            
            int pairID = getFadedColorPairID(peakHeights[bar], colorDecay[bar], colorPairIDs, color_decay_rate);
            // Scale bar height, with a multiplier to make quiet sounds visible
            int bar_height = std::min(channelHeight, static_cast<int>(peakHeights[bar] * channelHeight * 1.5f));
            int bar_width = base_bar_width + (bar < remainder ? 1 : 0); // Add remainder

            // Draw the bar
            if (bar_height > 0 && bar_width > 0 && current_x < width) {
                bar_width = std::min(bar_width, width - current_x); // Don't draw off-screen
                wattron(win, COLOR_PAIR(pairID));
                for (int col = 0; col < bar_width; col++) {
                    for (int y = 0; y < bar_height; y++) {
                        // Top channel draws from top-down, bottom channel draws from bottom-up
                        int y_pos = is_top_channel ? y_offset + channelHeight - 1 - y : y_offset + y;
                        mvwaddch(win, y_pos, current_x + col, ACS_BLOCK);
                    }
                }
                wattroff(win, COLOR_PAIR(pairID));
            }
            current_x += bar_width + (bar < num_bars - 1 ? spacing : 0);
        }
    };

    // Draw both channels
    process_channel(leftData, leftPeakHeights, leftColorDecay, 0, true); // Top (Left)
    process_channel(rightData, rightPeakHeights, rightColorDecay, channelHeight, false); // Bottom (Right)
}

/**
 * @brief Draws a simple circular/elliptical visualizer.
 *
 * This is a "Lissajous" style visualizer where the audio buffer is
 * wrapped around a circle. The amplitude of each sample determines its
 * distance (radius) from the center.
 */
void drawEllipse(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs) {
    int centerX = width / 2;
    int centerY = height / 2;
    const float PI = 3.1415926535f;
    float max_x_radius = (width / 2.0f) - 1;
    float max_y_radius = (height / 2.0f) - 1;

    // Iterate through each sample in the buffer
    for (int i = 0; i < BUFFER_FRAMES; ++i) {
        float mono_sample = (static_cast<float>(leftData[i]) + static_cast<float>(rightData[i])) / 2.0f;
        // 'radius' is the normalized amplitude
        float radius = std::abs(mono_sample) / 32767.0f;
        // 'angle' maps the sample's position to an angle
        float angle = (2.0f * PI * i) / BUFFER_FRAMES;
        
        // Calculate the point's position
        float y = radius * max_y_radius * sinf(angle) * 0.7f; // Y-axis squashed for aspect ratio
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

/**
 * @brief Draws a radial frequency visualizer ("Eclipse").
 *
 * Divides the 360-degree circle into angular bins (`num_points`).
 * The audio buffer is split into corresponding frequency bins.
 * The RMS (volume) of each frequency bin determines the radius at that angle.
 * The shape is smoothed to look less spiky.
 */
void drawEclipse(WINDOW *win, int width, int height, const int16_t* leftData, const int16_t* rightData, const std::vector<int>& colorPairIDs) {
    const int num_points = 128; // Number of angular/frequency bins
    static std::vector<float> pointAmplitudes(num_points, 0.0f); // Decaying peaks
    const float rise_factor = 0.5f;
    const float PI = 3.1415926535f;
    int centerX = width / 2;
    int centerY = height / 2;
    float max_radius = std::min(width / 3.0f, height / 2.0f);

    // Calculate RMS for each frequency bin and apply decay
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
        // Apply decay logic (rise fast, fall slow)
        if (current_rms > pointAmplitudes[i]) pointAmplitudes[i] += (current_rms - pointAmplitudes[i]) * rise_factor;
        else pointAmplitudes[i] = std::max(0.0f, pointAmplitudes[i] - decay__factor);
    }

    // --- Smoothing ---
    // This averages adjacent points to make the shape smoother.
    // It runs multiple passes for a softer look.
    const int smoothing_passes = 2;
    std::vector<float> tempAmplitudes(num_points);
    std::vector<float>* readBuffer = &pointAmplitudes;
    std::vector<float>* writeBuffer = &tempAmplitudes;
    for (int pass = 0; pass < smoothing_passes; ++pass) {
        for (int i = 0; i < num_points; ++i) {
            int prev_idx = (i == 0) ? num_points - 1 : i - 1;
            int next_idx = (i == num_points - 1) ? 0 : i + 1;
            // Weighted average: 25% prev, 50% current, 25% next
            (*writeBuffer)[i] = ((*readBuffer)[prev_idx] * 0.25f) + ((*readBuffer)[i] * 0.5f) + ((*readBuffer)[next_idx] * 0.25f);
        }
        std::swap(readBuffer, writeBuffer); // Swap buffers for next pass
    }
    if (readBuffer != &pointAmplitudes) pointAmplitudes = *readBuffer; // Ensure pointAmplitudes has final data

    // Draw the shape
    for (int i = 0; i < num_points; ++i) {
        float angle = 2.0f * PI * i / num_points;
        float amplitude = pointAmplitudes[i];
        
        // Calculate an outer and inner radius to give the shape "thickness"
        float radius = max_radius * (0.4f + amplitude * 1.5f);
        float inner_radius = max_radius * (0.2f + amplitude * 0.5f);
        
        int x = static_cast<int>(centerX + cosf(angle) * radius);
        int y = static_cast<int>(centerY + sinf(angle) * radius * 0.6); // Aspect ratio
        int inner_x = static_cast<int>(centerX + cosf(angle) * inner_radius);
        int inner_y = static_cast<int>(centerY + sinf(angle) * inner_radius * 0.6); // Aspect ratio
        
        int colorPairID = selectColorByAmplitude(amplitude, colorPairIDs);
        
        // Draw outer point
        if (x >= 0 && x < width && y >= 0 && y < height) {
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, y, x, ACS_DIAMOND);
            wattroff(win, COLOR_PAIR(colorPairID));
        }
        // Draw inner point
        if (inner_x >= 0 && inner_x < width && inner_y >= 0 && inner_y < height) {
            wattron(win, COLOR_PAIR(colorPairID));
            mvwaddch(win, inner_y, inner_x, '.');
            wattroff(win, COLOR_PAIR(colorPairID));
        }
    }
}

/**
 * @brief Toggles the VU Meter mode between PEAK and RMS.
 */
void toggleVuMeterMode(bool upArrow) {
    vuMeterMode = upArrow ? VU_PEAK : VU_RMS;
}

/**
 * @brief Gets the name of the current VU Meter mode for the status bar.
 */
const char* getVuMeterModeName() {
    return (vuMeterMode == VU_PEAK) ? "PEAK" : "RMS";
}
