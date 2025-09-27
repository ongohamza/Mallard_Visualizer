#include <ncurses.h>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <condition_variable>
#include "config_parser.h"

// Visualization modes
enum VisualizationMode {
    OSCILLOSCOPE,
    VU_METER,
    BAR_GRAPH,
    NUM_MODES  // Keep last for cycling
};

// Forward declarations for drawing functions from visualizer.cpp
extern void drawOscilloscope(int width, int height,
                             const int16_t* leftData, const int16_t* rightData,
                             const std::vector<int>& colorPairIDs, int edgePairID);
extern void drawVuMeter(int width, int height,
                        const int16_t* leftData, const int16_t* rightData,
                        const std::vector<int>& colorPairIDs, bool audio_active);
extern void drawBarGraph(int width, int height,
                         const int16_t* leftData, const int16_t* rightData,
                         const std::vector<int>& colorPairIDs, bool audio_active);

extern void toggleVuMeterMode(bool upArrow);
extern const char* getVuMeterModeName();

// --- Global State ---
const int SAMPLE_RATE = 44100;
const int TOTAL_SAMPLES = BUFFER_FRAMES * 2; // Stereo: 2 samples per frame
std::atomic<bool> running(true);
std::atomic<bool> audio_stream_active(false);
std::vector<int> colorPairIDs;
int edgePairID = 0;

// --- SPSC Lock-Free Ring Buffer ---
class RingBuffer {
public:
    RingBuffer() : m_head(0), m_tail(0) {}

    void write(const int16_t* data) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) % BUFFER_COUNT;
        if (next_head == m_tail.load(std::memory_order_acquire)) {
            // Consumer is lagging, but we push new data anyway, overwriting the oldest.
        }
        std::copy(data, data + TOTAL_SAMPLES, m_buffer[head]);
        m_head.store(next_head, std::memory_order_release);
    }

    bool read(int16_t* destLeft, int16_t* destRight) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);

        if (tail == head) return false; // No new data

        const int16_t* source_buffer = m_buffer[tail];
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            destLeft[i] = source_buffer[i * 2];
            destRight[i] = source_buffer[i * 2 + 1];
        }

        m_tail.store((tail + 1) % BUFFER_COUNT, std::memory_order_release);
        return true;
    }

private:
    static const int BUFFER_COUNT = 4;
    int16_t m_buffer[BUFFER_COUNT][TOTAL_SAMPLES] = {{0}};
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};

RingBuffer audioBuffer;

// --- Signal Handling & Audio Backend ---
#ifdef USE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
struct pw_main_loop *global_pw_loop = nullptr;
std::mutex loop_mutex;
#else // PulseAudio
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/channelmap.h>
#endif

void signal_handler(int) {
    running = false;
    #ifdef USE_PIPEWIRE
    std::lock_guard<std::mutex> lock(loop_mutex);
    if (global_pw_loop) {
        pw_main_loop_quit(global_pw_loop);
    }
    #endif
}

#ifdef USE_PIPEWIRE
// PipeWire audio capture implementation
struct PipeWireData {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
};
static void on_process(void *userdata) {
    struct pw_buffer *b;
    PipeWireData* data = static_cast<PipeWireData*>(userdata);
    if ((b = pw_stream_dequeue_buffer(data->stream)) != nullptr) {
        struct spa_buffer *buf = b->buffer;
        if (buf->datas[0].data) {
            size_t n_bytes = buf->datas[0].chunk->size;
            // A more robust check for available data size might be needed here
            // for different PipeWire buffer sizes, but this should work for typical settings.
            if (n_bytes >= sizeof(int16_t) * TOTAL_SAMPLES) {
                audioBuffer.write(static_cast<int16_t*>(buf->datas[0].data));
            }
        }
        pw_stream_queue_buffer(data->stream, b);
    }
}
static void on_state_changed(void *, enum pw_stream_state, enum pw_stream_state new_state, const char *) {
    switch (new_state) {
        case PW_STREAM_STATE_STREAMING: audio_stream_active = true; break;
        default: audio_stream_active = false; break;
    }
}
static const struct pw_stream_events stream_events = { .version = PW_VERSION_STREAM_EVENTS, .state_changed = on_state_changed, .process = on_process };
void audioCaptureThread() {
    pw_init(nullptr, nullptr);
    while (running) {
        PipeWireData data = {};
        data.loop = pw_main_loop_new(nullptr);
        { std::lock_guard<std::mutex> lock(loop_mutex); global_pw_loop = data.loop; }
        data.stream = pw_stream_new_simple(pw_main_loop_get_loop(data.loop), "Ncurses Visualizer", pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr), &stream_events, &data);
        
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        
        struct spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_S16;
        info.channels = 2;
        info.rate = SAMPLE_RATE;

        const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        pw_stream_connect(data.stream, PW_DIRECTION_INPUT, PW_ID_ANY, static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
        pw_main_loop_run(data.loop);
        { std::lock_guard<std::mutex> lock(loop_mutex); global_pw_loop = nullptr; }
        pw_stream_destroy(data.stream);
        pw_main_loop_destroy(data.loop);
        audio_stream_active = false;
        if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    pw_deinit();
}
#else // PulseAudio backend
void audioCaptureThread() {
    pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = SAMPLE_RATE, .channels = 2 };
    pa_buffer_attr buffer_attr = { .maxlength = (uint32_t)-1, .fragsize = (uint32_t)(TOTAL_SAMPLES * sizeof(int16_t)) };
    int error;
    while (running) {
        pa_simple *pa = pa_simple_new(nullptr, "Ncurses Visualizer", PA_STREAM_RECORD, nullptr, "record", &ss, nullptr, &buffer_attr, &error);
        if (!pa) {
            if (running) { std::this_thread::sleep_for(std::chrono::seconds(1)); }
            continue;
        }
        audio_stream_active = true;
        while (running) {
            int16_t buffer[TOTAL_SAMPLES];
            if (pa_simple_read(pa, buffer, sizeof(buffer), &error) < 0) break;
            audioBuffer.write(buffer);
        }
        audio_stream_active = false;
        pa_simple_free(pa);
    }
}
#endif

// --- ncurses UI and Main Application Logic ---
int initColors(const std::vector<std::pair<int, int>>& configPairs, std::pair<int, int> edgeColor) {
    start_color();
    use_default_colors();
    colorPairIDs.clear();
    for (int i = 0; i < static_cast<int>(configPairs.size()); ++i) {
        init_pair(i + 1, configPairs[i].first, configPairs[i].second);
        colorPairIDs.push_back(i + 1);
    }
    int edgeID = colorPairIDs.size() + 1;
    if (edgeColor.first != -2) {
        init_pair(edgeID, edgeColor.first, edgeColor.second);
    }
    return edgeID;
}

int main() {
    const char* home_dir_cstr = std::getenv("HOME");
    if (home_dir_cstr == nullptr) {
        std::cerr << "Error: HOME environment variable not found." << std::endl;
        return 1;
    }
    std::string full_path = std::string(home_dir_cstr) + "/.config/oscilloscope.conf";
    ConfigParser parser(full_path);
    
    std::vector<std::pair<int, int>> colorConfig;
    std::pair<int, int> edgeColorConfig;
    if (!parser.parse()) {
        fprintf(stderr, "Config Error: %s. Using default.\n", parser.getError().c_str());
        colorConfig = {{COLOR_GREEN, -1}};
        edgeColorConfig = {-2, -2};
    } else {
        colorConfig = parser.getColorPairs();
        edgeColorConfig = parser.getEdgeColorPair();
    }

    std::thread audioThread(audioCaptureThread);
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    if (has_colors()) { // <-- Corrected typo here
        edgePairID = initColors(colorConfig, edgeColorConfig);
    }
    
    // Set the default background for the entire window to ensure clean redraws.
    bkgd(' ' | COLOR_PAIR(0));

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    using namespace std::chrono;
    const auto frame_duration = microseconds(16667); // ~60 FPS
    auto next_frame_time = steady_clock::now();
    int16_t leftAudio[BUFFER_FRAMES] = {0};
    int16_t rightAudio[BUFFER_FRAMES] = {0};
    VisualizationMode currentMode = OSCILLOSCOPE;
    const char* modeNames[] = {"Oscilloscope", "VU Meter", "Bar Graph"};

    while (running) {
        next_frame_time += frame_duration;

        int ch = getch();
if (ch != ERR) {
    if (ch == 'q' || ch == 'Q') {
        running = false;
        #ifdef USE_PIPEWIRE
        std::lock_guard<std::mutex> lock(loop_mutex);
            if (global_pw_loop) pw_main_loop_quit(global_pw_loop);
                #endif
            }
            else if (ch == ' ') {
                currentMode = static_cast<VisualizationMode>((static_cast<int>(currentMode) + 1) % NUM_MODES);
            }
            else if (ch == KEY_UP && currentMode == VU_METER) {
                 toggleVuMeterMode(true); // Up arrow for PEAK mode
            }
            else if (ch == KEY_DOWN && currentMode == VU_METER) {
                toggleVuMeterMode(false); // Down arrow for RMS mode
            }
        }

        int width, height;
        getmaxyx(stdscr, height, width);
        erase();

        if (!audio_stream_active) {
            std::fill(leftAudio, leftAudio + BUFFER_FRAMES, 0);
            std::fill(rightAudio, rightAudio + BUFFER_FRAMES, 0);
        } else {
            audioBuffer.read(leftAudio, rightAudio);
        }

        int draw_height = height - 1;
        switch (currentMode) {
            case OSCILLOSCOPE: drawOscilloscope(width, draw_height, leftAudio, rightAudio, colorPairIDs, edgePairID); break;
            case VU_METER: drawVuMeter(width, draw_height, leftAudio, rightAudio, colorPairIDs, audio_stream_active); break;
            case BAR_GRAPH: drawBarGraph(width, draw_height, leftAudio, rightAudio, colorPairIDs, audio_stream_active); break;
            default: break;
        }
        
        attron(A_BOLD);
        mvprintw(0, 2, "L");
        mvprintw(draw_height / 2, 2, "R");
        attroff(A_BOLD);

        if (!audio_stream_active) {
            const char* msg1 = "Audio disconnected.";
            const char* msg2 = "Attempting to reconnect...";
            mvprintw(height / 2 - 1, (width - strlen(msg1)) / 2, "%s", msg1);
            mvprintw(height / 2, (width - strlen(msg2)) / 2, "%s", msg2);
        }

        static int frame_count = 0;
        static auto last_time = steady_clock::now();
        static float last_fps = 0.0f;
        frame_count++;
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_time).count() >= 1) {
            last_fps = frame_count;
            frame_count = 0;
            last_time = now;
        }

        attron(A_REVERSE);
        const char* vuModeInfo = (currentMode == VU_METER) ? getVuMeterModeName() : "N/A";
        mvprintw(height - 1, 0, " Status: %-12s | Mode: %-12s | VU: %-3s | FPS: %.0f | Press SPACE to change | Q to quit ",
         audio_stream_active ? "Connected" : "Disconnected", modeNames[currentMode], vuModeInfo, last_fps);
        attroff(A_REVERSE);

        refresh();

        auto sleep_duration = next_frame_time - steady_clock::now();
        if (sleep_duration.count() > 0) {
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    #ifdef USE_PIPEWIRE
    if (audioThread.joinable()) audioThread.join();
    #else
    if (audioThread.joinable()) audioThread.detach();
    #endif
    endwin();
    return 0;
}
