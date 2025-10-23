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
#include <pthread.h>
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
const int SAMPLE_RATE = 44100;
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
        if (next_head == m_tail.load(std::memory_order_acquire)) {} // Overwrite
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
    static const int BUFFER_COUNT = 4;
    int16_t m_buffer[BUFFER_COUNT][TOTAL_SAMPLES] = {{0}};
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};
RingBuffer audioBuffer;

#ifdef USE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
struct pw_main_loop *global_pw_loop = nullptr;
std::mutex loop_mutex;
#else // PulseAudio
#include <pulse/pulseaudio.h>
pa_mainloop* global_pa_loop = nullptr;
std::mutex loop_mutex;
#endif

void shutdown_program(std::thread& audioThread);

void signal_handler(int) {
    running = false; // The main loops will see this and exit
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
        data.stream = pw_stream_new_simple(
            pw_main_loop_get_loop(data.loop), "Ncurses Visualizer",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, "ncurses_visualizer_capture",
                PW_KEY_STREAM_CAPTURE_SINK, "true", nullptr),
            &stream_events, &data);
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_S16;
        info.channels = 2;
        info.rate = SAMPLE_RATE;
        const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        pw_stream_connect(data.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        pw_main_loop_run(data.loop);
        { std::lock_guard<std::mutex> lock(loop_mutex); global_pw_loop = nullptr; }
        pw_stream_destroy(data.stream);
        pw_main_loop_destroy(data.loop);
        audio_stream_active = false;
        if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    pw_deinit();
}
#else // PulseAudio backend (FIXED ASYNCHRONOUS VERSION)

// --- PulseAudio Asynchronous Capture Implementation ---
struct PulseData {
    pa_mainloop* mainloop;
    pa_stream* stream;
};

// This callback is where we read audio data from the stream
static void stream_read_callback(pa_stream* s, size_t length, void* userdata) {
    const void* data;
    if (pa_stream_peek(s, &data, &length) < 0) {
        return;
    }
    if (data && length > 0) {
        // We only need one buffer's worth of data at a time
        size_t to_write = std::min(length, sizeof(int16_t) * TOTAL_SAMPLES);
        if (to_write >= sizeof(int16_t) * TOTAL_SAMPLES) {
             audioBuffer.write(static_cast<const int16_t*>(data));
        }
    }
    // Discard the data we've processed from the buffer
    if (length > 0) {
        pa_stream_drop(s);
    }
}

// This callback monitors the state of the stream (e.g., connected, failed)
static void stream_state_callback(pa_stream* s, void* userdata) {
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
            audio_stream_active = true;
            break;
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            audio_stream_active = false;
            if (userdata) {
                pa_mainloop_quit(static_cast<PulseData*>(userdata)->mainloop, 1);
            }
            break;
        default:
            break;
    }
}

void audioCaptureThread() {
    while (running) {
        PulseData data = {};
        data.mainloop = pa_mainloop_new();
        if (!data.mainloop) {
            if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(loop_mutex);
            global_pa_loop = data.mainloop;
        }

        pa_mainloop_api* api = pa_mainloop_get_api(data.mainloop);
        pa_context* context = pa_context_new(api, "Ncurses Visualizer");
        pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
        
        // Wait for context to be ready
        bool context_ready = false;
        while (running) {
            pa_context_state_t state = pa_context_get_state(context);
            if (state == PA_CONTEXT_READY) {
                context_ready = true;
                break;
            }
            if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
                // context_ready remains false
                break;
            }
            pa_mainloop_iterate(data.mainloop, 0, nullptr);
        }

        // ** FIX IS HERE: Replaced the 'goto' with this block **
        // If context failed to connect, clean up and restart the main loop.
        if (!context_ready) {
            pa_context_unref(context);
            pa_mainloop_free(data.mainloop);
            if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
            continue; // Skips the rest of this iteration and tries again.
        }

        pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = SAMPLE_RATE, .channels = 2 };
        pa_buffer_attr buffer_attr = { .maxlength = (uint32_t)-1, .fragsize = (uint32_t)(TOTAL_SAMPLES * sizeof(int16_t)) };

        data.stream = pa_stream_new(context, "record", &ss, nullptr);
        pa_stream_set_state_callback(data.stream, stream_state_callback, &data);
        pa_stream_set_read_callback(data.stream, stream_read_callback, &data);
        pa_stream_connect_record(data.stream, nullptr, &buffer_attr, static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY));
        
        // This is the main event loop, it will block here until quit
        pa_mainloop_run(data.mainloop, nullptr);
        
        // --- Cleanup ---
        audio_stream_active = false;
        {
            std::lock_guard<std::mutex> lock(loop_mutex);
            global_pa_loop = nullptr;
        }
        
        if (pa_stream_get_state(data.stream) != PA_STREAM_UNCONNECTED) {
            pa_stream_disconnect(data.stream);
        }
        pa_stream_unref(data.stream);
        pa_context_disconnect(context);
        pa_context_unref(context);
        pa_mainloop_free(data.mainloop);
        
        if (running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
#endif

void shutdown_program(std::thread& audioThread) {
    running = false;
    #ifdef USE_PIPEWIRE
    std::lock_guard<std::mutex> lock(loop_mutex);
    if (global_pw_loop) pw_main_loop_quit(global_pw_loop);
    #else // PulseAudio
    std::lock_guard<std::mutex> lock(loop_mutex);
    if (global_pa_loop) pa_mainloop_quit(global_pa_loop, 0);
    #endif
}

int initColors(const std::vector<std::pair<int, int>>& configPairs) {
    start_color();
    use_default_colors();
    colorPairIDs.clear();
    for (size_t i = 0; i < configPairs.size(); ++i) {
        init_pair(i + 1, configPairs[i].first, configPairs[i].second);
        colorPairIDs.push_back(i + 1);
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
    
    std::thread audioThread(audioCaptureThread);
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
    const auto frame_duration = microseconds(16667);
    auto next_frame_time = steady_clock::now();
    int16_t leftAudio[BUFFER_FRAMES] = {0};
    int16_t rightAudio[BUFFER_FRAMES] = {0};
    
    std::vector<std::string> modeNames;
    modeNames.push_back("Oscilloscope");
    modeNames.push_back("VU Meter");
    modeNames.push_back("Bar Graph");
    modeNames.push_back("Galaxy");
    modeNames.push_back("Ellipse");
    modeNames.push_back("Eclipse");
    for(const auto& viz : customVisualizers) {
        modeNames.push_back(viz.name);
    }
    const int total_modes = modeNames.size();
    int currentModeIdx = 0;

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
                shutdown_program(audioThread);
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

        if (!audio_stream_active) {
            std::fill(leftAudio, leftAudio + BUFFER_FRAMES, 0);
            std::fill(rightAudio, rightAudio + BUFFER_FRAMES, 0);
        } else {
            audioBuffer.read(leftAudio, rightAudio);
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

        if (!audio_stream_active) {
            const char* msg1 = "Audio disconnected.";
            const char* msg2 = "Attempting to reconnect...";
            mvprintw(height / 2 - 1, (width - strlen(msg1)) / 2, "%s", msg1);
            mvprintw(height / 2, (width - strlen(msg2)) / 2, "%s", msg2);
        }

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

        attron(A_REVERSE);
        mvprintw(height - 1, 0, "%*s", width, " ");
        const char* vuModeInfo = (currentModeIdx == VU_METER) ? getVuMeterModeName() : "N/A";
        mvprintw(height - 1, 0, " Status: %-12s | Mode: %-12s | VU: %-3s | FPS: %.0f | Press SPACE to change | Q to quit ",
                 audio_stream_active ? "Connected" : "Disconnected", modeNames[currentModeIdx].c_str(), vuModeInfo, last_fps);
        attroff(A_REVERSE);

        refresh();

        auto sleep_duration = next_frame_time - std::chrono::steady_clock::now();
        if (sleep_duration.count() > 0) {
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    if (audioThread.joinable()) {
        audioThread.join();
    }
    delwin(vis_win);
    endwin();
    return 0;
}
