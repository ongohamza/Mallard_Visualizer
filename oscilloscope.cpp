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
#include "visualizer.h"

// Built-in modes
enum BuiltInMode {
    OSCILLOSCOPE,
    VU_METER,
    BAR_GRAPH,
    GALAXY,
    ELLIPSE,
    ECLIPSE,
    NUM_BUILT_IN_MODES
};

// --- Global State ---
const int DEFAULT_SAMPLE_RATE = 44100;
std::atomic<uint32_t> global_sample_rate(DEFAULT_SAMPLE_RATE);
const int TOTAL_SAMPLES = BUFFER_FRAMES * 2;
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
            // Buffer full, overwrite oldest
        } 
        std::copy(data, data + TOTAL_SAMPLES, m_buffer[head]);
        m_head.store(next_head, std::memory_order_release);
    }
    bool read(int16_t* destLeft, int16_t* destRight) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) return false;
        const int16_t* source_buffer = m_buffer[tail];
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            destLeft[i] = source_buffer[i * 2];
            destRight[i] = source_buffer[i * 2 + 1];
        }
        m_tail.store((tail + 1) % BUFFER_COUNT, std::memory_order_release);
        return true;
    }
private:
    static const int BUFFER_COUNT = 8; // Increased buffer count for stability
    int16_t m_buffer[BUFFER_COUNT][TOTAL_SAMPLES] = {{0}};
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};
RingBuffer audioBuffer;

#ifdef USE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
struct pw_main_loop *global_pw_loop = nullptr;
std::mutex loop_mutex;
#else // PulseAudio
#include <pulse/pulseaudio.h>
#include <pulse/error.h>
pa_mainloop* global_pa_loop = nullptr;
std::mutex loop_mutex;
#endif

void signal_handler(int) {
    running = false;
}

#ifdef USE_PIPEWIRE
// --- PipeWire Audio Capture Implementation ---
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
            if (n_bytes >= sizeof(int16_t) * TOTAL_SAMPLES) {
                audioBuffer.write(static_cast<int16_t*>(buf->datas[0].data));
                if(!audio_stream_active) audio_stream_active = true;
            }
        }
        pw_stream_queue_buffer(data->stream, b);
    }
}

static void on_state_changed(void *, enum pw_stream_state, enum pw_stream_state new_state, const char *error) {
    switch (new_state) {
        case PW_STREAM_STATE_STREAMING: 
            audio_stream_active = true; 
            break;
        case PW_STREAM_STATE_ERROR:
            audio_stream_active = false;
            std::cerr << "PipeWire Stream Error: " << (error ? error : "Unknown") << std::endl;
            break;
        default: 
            audio_stream_active = false; 
            break;
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_audio_info_raw info;
    if (spa_format_audio_raw_parse(param, &info) >= 0) {
        if (info.rate > 0) {
            global_sample_rate.store(info.rate, std::memory_order_relaxed);
        }
    }
}

static const struct pw_stream_events stream_events = { 
    .version = PW_VERSION_STREAM_EVENTS, 
    .state_changed = on_state_changed, 
    .param_changed = on_param_changed,
    .process = on_process 
};

void audioCaptureThread() {
    pw_init(nullptr, nullptr);
    while (running) {
        PipeWireData data = {};
        data.loop = pw_main_loop_new(nullptr);
        
        { std::lock_guard<std::mutex> lock(loop_mutex); global_pw_loop = data.loop; }

        data.stream = pw_stream_new_simple(
            pw_main_loop_get_loop(data.loop), "Ncurses Visualizer",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", 
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Music", 
                PW_KEY_NODE_NAME, "visualizer_capture",
                PW_KEY_STREAM_CAPTURE_SINK, "true", // Crucial for system audio
                nullptr),
            &stream_events, &data);

        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_S16;
        info.channels = 2;
        info.rate = DEFAULT_SAMPLE_RATE;

        const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        
        int ret = pw_stream_connect(data.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            params, 1);

        if (ret < 0) {
            std::cerr << "Failed to connect PipeWire stream: " << strerror(ret) << std::endl;
        } else {
            pw_main_loop_run(data.loop);
        }

        { std::lock_guard<std::mutex> lock(loop_mutex); global_pw_loop = nullptr; }
        
        if(data.stream) pw_stream_destroy(data.stream);
        if(data.loop) pw_main_loop_destroy(data.loop);
        
        audio_stream_active = false;
        if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    pw_deinit();
}

#else // --- PulseAudio (Explicit Monitor) ---

struct PulseData {
    pa_mainloop* mainloop;
    pa_context* context;
    pa_stream* stream;
    std::string monitor_name;
};

static void stream_read_callback(pa_stream* s, size_t length, void* userdata) {
    const void* data;
    if (pa_stream_peek(s, &data, &length) < 0) return;
    if (data && length > 0) {
        size_t to_write = std::min(length, sizeof(int16_t) * TOTAL_SAMPLES);
        if (to_write >= sizeof(int16_t) * TOTAL_SAMPLES) {
             audioBuffer.write(static_cast<const int16_t*>(data));
             if(!audio_stream_active) audio_stream_active = true;
        }
    }
    if (length > 0) pa_stream_drop(s);
}

static void stream_state_callback(pa_stream* s, void* userdata) {
    PulseData* data = static_cast<PulseData*>(userdata);
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
            audio_stream_active = true;
            global_sample_rate.store(DEFAULT_SAMPLE_RATE, std::memory_order_relaxed);
            break;
        case PA_STREAM_FAILED:
            std::cerr << "PulseAudio Stream Failed: " << pa_strerror(pa_context_errno(data->context)) << std::endl;
            audio_stream_active = false;
            pa_mainloop_quit(data->mainloop, 1);
            break;
        case PA_STREAM_TERMINATED:
            audio_stream_active = false;
            pa_mainloop_quit(data->mainloop, 0);
            break;
        default: break;
    }
}

static void server_info_callback(pa_context* c, const pa_server_info* i, void* userdata) {
    PulseData* data = static_cast<PulseData*>(userdata);
    
    // Fallback logic: Try monitor of default sink, otherwise use default source (mic)
    const char* target_device = nullptr;
    if (i && i->default_sink_name) {
        data->monitor_name = std::string(i->default_sink_name) + ".monitor";
        target_device = data->monitor_name.c_str();
    } else {
        std::cerr << "No default sink found. Attempting default source (mic)..." << std::endl;
        target_device = nullptr; // Records from default source
    }

    pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = DEFAULT_SAMPLE_RATE, .channels = 2 };
    // Adjust buffer attributes for stability
    pa_buffer_attr buffer_attr;
    buffer_attr.maxlength = (uint32_t) -1;
    buffer_attr.tlength = (uint32_t) -1;
    buffer_attr.prebuf = (uint32_t) -1;
    buffer_attr.minreq = (uint32_t) -1;
    buffer_attr.fragsize = (uint32_t)(TOTAL_SAMPLES * sizeof(int16_t)); // Important for visualizer latency

    data->stream = pa_stream_new(data->context, "VisualizerCapture", &ss, nullptr);
    if (!data->stream) {
        std::cerr << "Failed to create stream: " << pa_strerror(pa_context_errno(c)) << std::endl;
        pa_mainloop_quit(data->mainloop, 1);
        return;
    }

    pa_stream_set_state_callback(data->stream, stream_state_callback, userdata);
    pa_stream_set_read_callback(data->stream, stream_read_callback, userdata);
    
    int ret = pa_stream_connect_record(data->stream, target_device, &buffer_attr, 
        static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE));
        
    if (ret < 0) {
        std::cerr << "Failed to connect record: " << pa_strerror(pa_context_errno(c)) << std::endl;
        pa_mainloop_quit(data->mainloop, 1);
    }
}

static void context_state_callback(pa_context* c, void* userdata) {
    PulseData* data = static_cast<PulseData*>(userdata);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa_context_get_server_info(c, server_info_callback, userdata);
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            std::cerr << "PulseAudio Context Failed/Terminated" << std::endl;
            pa_mainloop_quit(data->mainloop, 1);
            break;
        default: break;
    }
}

void audioCaptureThread() {
    while (running) {
        PulseData data = {};
        data.mainloop = pa_mainloop_new();
        if (!data.mainloop) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        { std::lock_guard<std::mutex> lock(loop_mutex); global_pa_loop = data.mainloop; }

        pa_mainloop_api* api = pa_mainloop_get_api(data.mainloop);
        data.context = pa_context_new(api, "Ncurses Visualizer");
        
        pa_context_set_state_callback(data.context, context_state_callback, &data);
        
        if (pa_context_connect(data.context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            std::cerr << "PulseAudio connect failed." << std::endl;
            pa_context_unref(data.context);
            pa_mainloop_free(data.mainloop);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        pa_mainloop_run(data.mainloop, nullptr);
        
        // Cleanup sequence
        { std::lock_guard<std::mutex> lock(loop_mutex); global_pa_loop = nullptr; }
        
        if (data.stream) {
            pa_stream_disconnect(data.stream);
            pa_stream_unref(data.stream);
        }
        if (data.context) {
            pa_context_disconnect(data.context);
            pa_context_unref(data.context);
        }
        pa_mainloop_free(data.mainloop);
        
        audio_stream_active = false;
        if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
#endif

int initColors(const std::vector<std::pair<int, int>>& configPairs) {
    if (!has_colors()) return 0;
    start_color();
    use_default_colors();
    colorPairIDs.clear();
    for (size_t i = 0; i < configPairs.size(); ++i) {
        init_pair(i + 1, configPairs[i].first, configPairs[i].second);
        colorPairIDs.push_back(i + 1);
    }
    // Fallback if config is empty to ensure visibility
    if (colorPairIDs.empty()) {
        init_pair(1, COLOR_WHITE, COLOR_BLACK);
        colorPairIDs.push_back(1);
    }
    return colorPairIDs.size() + 2;
}

int main() {
    const char* home_dir_cstr = std::getenv("HOME");
    if (home_dir_cstr == nullptr) {
        std::cerr << "Error: HOME environment variable not found." << std::endl;
        return 1;
    }
    std::string full_path = std::string(home_dir_cstr) + "/.config/oscilloscope.conf";
    ConfigParser parser(full_path);
    parser.parse();
    
    auto colorConfig = parser.getColorPairs();
    auto customVisualizers = parser.getCustomVisualizers();
    
    // Start Audio Thread First
    std::thread audioThread(audioCaptureThread);

    // Initialize Ncurses
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE); 
    
    if (has_colors()) {
        edgePairID = initColors(colorConfig);
    }

    bkgd(' ' | COLOR_PAIR(0));
    refresh();

    int height, width;
    getmaxyx(stdscr, height, width);
    WINDOW *vis_win = newwin(height - 1, width, 0, 0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    using namespace std::chrono;
    const auto frame_duration = microseconds(16667); // ~60 FPS
    auto next_frame_time = steady_clock::now();
    int16_t leftAudio[BUFFER_FRAMES] = {0};
    int16_t rightAudio[BUFFER_FRAMES] = {0};
    
    std::vector<std::string> modeNames = {
        "Oscilloscope", "VU Meter", "Bar Graph", "Galaxy", "Ellipse", "Eclipse"
    };
    for(const auto& viz : customVisualizers) {
        modeNames.push_back(viz.name);
    }
    const int total_modes = modeNames.size();
    int currentModeIdx = 0;

    // --- Main Rendering Loop ---
    while (running) {
        next_frame_time += frame_duration;
        int ch = getch();
        if (ch != ERR) {
            if (ch == KEY_RESIZE) {
                getmaxyx(stdscr, height, width);
                wresize(vis_win, height - 1, width);
                bkgd(' ' | COLOR_PAIR(0));
                touchwin(stdscr);
                refresh();
            } else if (ch == 'q' || ch == 'Q') {
                running = false;
            } else if (ch == ' ') {
                currentModeIdx = (currentModeIdx + 1) % total_modes;
            } else if (ch == KEY_UP && currentModeIdx == VU_METER) {
                 toggleVuMeterMode(true);
            } else if (ch == KEY_DOWN && currentModeIdx == VU_METER) {
                toggleVuMeterMode(false);
            }
        }

        if (!running) break;

        werase(vis_win);

        bool has_new_data = audioBuffer.read(leftAudio, rightAudio);

        if (!audio_stream_active || !has_new_data) {
            // Decay / Silence
            std::fill(leftAudio, leftAudio + BUFFER_FRAMES, 0);
            std::fill(rightAudio, rightAudio + BUFFER_FRAMES, 0);
        }

        int vis_height, vis_width;
        getmaxyx(vis_win, vis_height, vis_width);

        if (currentModeIdx < NUM_BUILT_IN_MODES) {
            switch(static_cast<BuiltInMode>(currentModeIdx)) {
                case OSCILLOSCOPE: drawOscilloscope(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs, edgePairID); break;
                case VU_METER: drawVuMeter(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs, audio_stream_active); break;
                case BAR_GRAPH: drawBarGraph(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs, audio_stream_active); break;
                case GALAXY: drawGalaxy(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs, audio_stream_active); break;
                case ELLIPSE: drawEllipse(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs); break;
                case ECLIPSE: drawEclipse(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs); break;
                default: break;
            }
        } else {
            int custom_idx = currentModeIdx - NUM_BUILT_IN_MODES;
            if (custom_idx < static_cast<int>(customVisualizers.size())) {
                drawCustomShape(vis_win, vis_width, vis_height, leftAudio, rightAudio, colorPairIDs, customVisualizers[custom_idx]);
            }
        }

        if (currentModeIdx == OSCILLOSCOPE || currentModeIdx == VU_METER || currentModeIdx == BAR_GRAPH) {
            wattron(vis_win, A_BOLD);
            mvwprintw(vis_win, 0, 2, "L");
            mvwprintw(vis_win, vis_height / 2, 2, "R");
            wattroff(vis_win, A_BOLD);
        }

        wrefresh(vis_win);

        // UI Status Bar
        static int frame_count = 0;
        static auto last_time = std::chrono::steady_clock::now();
        static float last_fps = 0.0f;
        frame_count++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count() >= 1) {
            last_fps = frame_count;
            frame_count = 0;
            last_time = now;
        }

        std::string rate_str = std::to_string(global_sample_rate.load(std::memory_order_relaxed));
        attron(A_REVERSE);
        mvprintw(height - 1, 0, "%*s", width, " ");
        const char* vuModeInfo = (currentModeIdx == VU_METER) ? getVuMeterModeName() : "N/A";
        mvprintw(height - 1, 0, " Rate: %-5s | %-12s | %-12s | VU: %-3s | FPS: %.0f | SPACE: Cycle | Q: Quit",
                 rate_str.c_str(),
                 audio_stream_active ? "Connected" : "Disconnected",
                 modeNames[currentModeIdx].c_str(), 
                 vuModeInfo, 
                 last_fps);
        attroff(A_REVERSE);

        refresh();

        auto sleep_duration = next_frame_time - std::chrono::steady_clock::now();
        if (sleep_duration.count() > 0) {
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    // --- CLEANUP SEQUENCE ---
    // 1. Force Audio Loop to Wake/Quit
    {
        std::lock_guard<std::mutex> lock(loop_mutex);
        #ifdef USE_PIPEWIRE
        if (global_pw_loop) pw_main_loop_quit(global_pw_loop);
        #else
        if (global_pa_loop) pa_mainloop_quit(global_pa_loop, 0);
        #endif
    }

    // 2. Join Thread (Safe because loop was signaled)
    if (audioThread.joinable()) {
        audioThread.join();
    }

    // 3. Destroy Ncurses
    delwin(vis_win);
    delwin(stdscr);
    endwin();

    return 0;
}
