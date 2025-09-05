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

// --- Conditional Includes for Audio Backend ---
#ifdef USE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#else // Default to PulseAudio
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/channelmap.h>
#endif

// Visualization modes
enum VisualizationMode {
    OSCILLOSCOPE,
    VU_METER,
    BAR_GRAPH,
    NUM_MODES  // Keep last for cycling
};

// Forward declarations for visualization functions from visualizer.cpp
extern void drawOscilloscope(int width, int height,
                       const int16_t* leftData, const int16_t* rightData,
                       const std::vector<int>& colorPairIDs, int edgePairID);

extern void drawVuMeter(int width, int height,
                        const int16_t* leftData, const int16_t* rightData,
                        const std::vector<int>& colorPairIDs);

extern void drawBarGraph(int width, int height,
                         const int16_t* leftData, const int16_t* rightData,
                         const std::vector<int>& colorPairIDs);


// --- Globals ---
const int SAMPLE_RATE = 44100;
const int TOTAL_SAMPLES = BUFFER_FRAMES * 2; // Stereo: 2 samples per frame
std::atomic<bool> running(true);
std::vector<int> colorPairIDs;
int edgePairID = 0;

// --- Thread-safe Audio Buffer (Unchanged) ---
class AudioBuffer {
public:
    void write(const int16_t* data) {
        std::lock_guard<std::mutex> lock(mutex);
        std::copy(data, data + TOTAL_SAMPLES, buffer);
        newData = true;
    }

    bool read(int16_t* destLeft, int16_t* destRight) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!newData) return false;

        for (int i = 0; i < BUFFER_FRAMES; i++) {
            destLeft[i] = buffer[i * 2];
            destRight[i] = buffer[i * 2 + 1];
        }
        newData = false;
        return true;
    }

private:
    int16_t buffer[TOTAL_SAMPLES] = {0};
    std::mutex mutex;
    bool newData = false;
};

AudioBuffer audioBuffer;


// --- Conditional Audio Capture Implementation ---

#ifdef USE_PIPEWIRE // PIPEWIRE IMPLEMENTATION
// --- PipeWire Audio Capture Data Structure ---
struct PipeWireData {
    pw_main_loop *loop;
    pw_stream *stream;
};

// Global loop pointer for clean shutdown
static pw_main_loop *g_main_loop = nullptr;

static void on_process(void *userdata) {
    PipeWireData *data = static_cast<PipeWireData*>(userdata);
    pw_buffer *b;
    if ((b = pw_stream_dequeue_buffer(data->stream)) == nullptr) return;

    spa_buffer *buf = b->buffer;
    if (buf->datas[0].data != nullptr) {
        uint32_t size = buf->datas[0].chunk->size;
        if (size >= TOTAL_SAMPLES * sizeof(int16_t)) {
            audioBuffer.write(static_cast<int16_t*>(buf->datas[0].data));
        }
    }
    pw_stream_queue_buffer(data->stream, b);
}

static void on_stream_state_changed(void *, pw_stream_state, pw_stream_state state, const char *error) {
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "PipeWire Stream error: %s\n", error);
        running = false;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_process,
};

void audioCaptureThread() {
    PipeWireData data = {};
    const spa_pod *params[1];
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    pw_init(nullptr, nullptr);
    data.loop = pw_main_loop_new(nullptr);
    g_main_loop = data.loop;

    data.stream = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop), "ncurses-visualizer",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr),
        &stream_events, &data);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16, .channels = 2, .rate = SAMPLE_RATE));

    pw_stream_connect(data.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

    pw_main_loop_run(data.loop); // This blocks until quit

    g_main_loop = nullptr;
    pw_stream_destroy(data.stream);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
}

#else // PULSEAUDIO IMPLEMENTATION (Default)

void audioCaptureThread() {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = SAMPLE_RATE;
    ss.channels = 2;

    pa_channel_map map;
    pa_channel_map_init_stereo(&map);

    int error;
    pa_simple *s = pa_simple_new(nullptr, "Ncurses Visualizer", PA_STREAM_RECORD, nullptr, "record", &ss, &map, nullptr, &error);

    if (!s) {
        fprintf(stderr, "PulseAudio error: %s\n", pa_strerror(error));
        running = false;
        return;
    }

    while (running) {
        int16_t buffer[TOTAL_SAMPLES];
        if (pa_simple_read(s, buffer, sizeof(buffer), &error) < 0) {
            fprintf(stderr, "PulseAudio read error: %s\n", pa_strerror(error));
            running = false;
            continue;
        }
        audioBuffer.write(buffer);
    }

    if (s) pa_simple_free(s);
}
#endif

// --- Main Application Logic (Mostly Unchanged) ---

void signal_handler(int) {
    running = false;
#ifdef USE_PIPEWIRE
    if (g_main_loop) {
        pw_main_loop_quit(g_main_loop);
    }
#endif
}

int initColors(const std::vector<std::pair<int, int>>& configPairs, std::pair<int, int> edgeColor) {
    start_color();
    use_default_colors();
    colorPairIDs.clear();
    for (int i = 0; i < static_cast<int>(configPairs.size()); ++i) {
        init_pair(i + 1, configPairs[i].first, configPairs[i].second);
        colorPairIDs.push_back(i + 1);
    }
    int edgeID = colorPairIDs.size() + 1;
    if (edgeColor.first == -2) init_pair(edgeID, COLOR_RED, -1);
    else init_pair(edgeID, edgeColor.first, edgeColor.second);
    return edgeID;
}

int main() {
    ConfigParser parser("/home/hamza/.config/oscilloscope.conf");
    std::vector<std::pair<int, int>> colorConfig;
    std::pair<int, int> edgeColorConfig;

    if (!parser.parse()) {
        fprintf(stderr, "Config Error: %s\n", parser.getError().c_str());
        fprintf(stderr, "Using default color (green)\n");
        colorConfig = { {COLOR_GREEN, -1} };
        edgeColorConfig = {-2, -2};
    } else {
        colorConfig = parser.getColorPairs();
        edgeColorConfig = parser.getEdgeColorPair();
    }
    
    std::thread audioThread(audioCaptureThread);

    initscr();
    cbreak(); noecho(); curs_set(0); nodelay(stdscr, TRUE); keypad(stdscr, TRUE);
    if (has_colors()) edgePairID = initColors(colorConfig, edgeColorConfig);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    using namespace std::chrono;
    const auto frame_duration = microseconds(16667);
    auto next_frame_time = steady_clock::now();
    int16_t leftAudio[BUFFER_FRAMES] = {0}, rightAudio[BUFFER_FRAMES] = {0};
    int total_frames = 0;
    auto last_info_update_time = steady_clock::now();
    float last_fps = 0.0f;
    VisualizationMode currentMode = OSCILLOSCOPE;
    const char* modeNames[] = {"Oscilloscope", "VU Meter", "Bar Graph"};

    while (running) {
        next_frame_time += frame_duration;
        if (!audioBuffer.read(leftAudio, rightAudio)) {
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }

        int ch = getch();
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') running = false;
            else if (ch == ' ') currentMode = static_cast<VisualizationMode>((static_cast<int>(currentMode) + 1) % NUM_MODES);
        }

        int width, height;
        getmaxyx(stdscr, height, width);
        int draw_height = height - 1; 

        erase();
        switch (currentMode) {
            case OSCILLOSCOPE: drawOscilloscope(width, draw_height, leftAudio, rightAudio, colorPairIDs, edgePairID); break;
            case VU_METER: drawVuMeter(width, draw_height, leftAudio, rightAudio, colorPairIDs); break;
            case BAR_GRAPH: drawBarGraph(width, draw_height, leftAudio, rightAudio, colorPairIDs); break;
        }
        
        attron(A_BOLD);
        mvprintw(0, 2, "L");
        mvprintw(draw_height / 2, 2, "R");
        attroff(A_BOLD);

        total_frames++;
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_info_update_time).count() >= 1) {
            last_fps = total_frames / duration_cast<duration<float>>(now - last_info_update_time).count();
            total_frames = 0;
            last_info_update_time = now;
        }
        
        attron(A_REVERSE);
        mvprintw(height - 1, 0, " Mode: %s | Freq: %dHz | FPS: %.1f | Press SPACE to change | Q to quit ", modeNames[currentMode], SAMPLE_RATE, last_fps);
        int curX, curY;
        getyx(stdscr, curY, curX);
        for(int i = curX; i < width; ++i) addch(' ');
        attroff(A_REVERSE);

        refresh();

        auto sleep_duration = next_frame_time - steady_clock::now();
        if (sleep_duration.count() > 0) std::this_thread::sleep_for(sleep_duration);
    }

    signal_handler(0); // Ensure loops are quit
    if (audioThread.joinable()) audioThread.join();
    
    endwin();
    return 0;
}
