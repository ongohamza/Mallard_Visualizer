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

// Visualization modes
enum VisualizationMode {
    OSCILLOSCOPE,
    VU_METER,
    BAR_GRAPH,
    NUM_MODES  // Keep last for cycling
};

extern void drawOscilloscope(int width, int height,
                       const int16_t* leftData, const int16_t* rightData,
                       const std::vector<int>& colorPairIDs, int edgePairID);
extern void drawVuMeter(int width, int height,
                        const int16_t* leftData, const int16_t* rightData,
                        const std::vector<int>& colorPairIDs);
extern void drawBarGraph(int width, int height,
                         const int16_t* leftData, const int16_t* rightData,
                         const std::vector<int>& colorPairIDs);

const int SAMPLE_RATE = 44100;
const int TOTAL_SAMPLES = BUFFER_FRAMES * 2; // Stereo: 2 samples per frame
std::atomic<bool> running(true);
std::vector<int> colorPairIDs;
int edgePairID = 0;

// --- New Lock-Free Ring Buffer Implementation ---
// This is designed for a single producer (audio thread) and single consumer (UI thread).
class RingBuffer {
public:
    RingBuffer() : m_head(0), m_tail(0) {}

    // Called by the audio thread. This is now extremely fast.
    void write(const int16_t* data) {
        // Get the current write position.
        size_t head = m_head.load(std::memory_order_relaxed);
        
        // Copy the data into the buffer at the head position.
        std::copy(data, data + TOTAL_SAMPLES, m_buffer[head]);
        
        // Atomically advance the head pointer. This signals to the reader
        // that new data is available. The reader will see this change.
        m_head.store((head + 1) % BUFFER_COUNT, std::memory_order_release);
    }

    // Called by the UI thread.
    bool read(int16_t* destLeft, int16_t* destRight) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);

        if (tail == head) {
            return false; // Buffer is empty, no new data.
        }

        // We can now safely read from the tail position without any locks.
        const int16_t* source_buffer = m_buffer[tail];
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            destLeft[i] = source_buffer[i * 2];
            destRight[i] = source_buffer[i * 2 + 1];
        }

        // Atomically advance the tail pointer to consume the data.
        m_tail.store((tail + 1) % BUFFER_COUNT, std::memory_order_release);
        return true;
    }

private:
    // We use a small ring buffer of 4 audio chunks.
    // This gives plenty of slack between the writer and reader.
    static const int BUFFER_COUNT = 4; 
    int16_t m_buffer[BUFFER_COUNT][TOTAL_SAMPLES] = {{0}};
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};

RingBuffer audioBuffer;

void signal_handler(int) {
    running = false;
}

#ifdef USE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>

struct PipeWireData {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
};

static void on_process(void *userdata) {
    struct pw_buffer *b;
    if ((b = pw_stream_dequeue_buffer(static_cast<PipeWireData*>(userdata)->stream)) == nullptr) {
        pw_log_warn("out of buffers: %m");
        return;
    }
    struct spa_buffer *buf = b->buffer;
    if (buf->datas[0].data) {
        audioBuffer.write(static_cast<int16_t*>(buf->datas[0].data));
    }
    pw_stream_queue_buffer(static_cast<PipeWireData*>(userdata)->stream, b);
}

static const struct pw_stream_events stream_events = { PW_VERSION_STREAM_EVENTS, .process = on_process };

void audioCaptureThread() {
    PipeWireData data = {};
    pw_init(nullptr, nullptr);
    data.loop = pw_main_loop_new(nullptr);
    data.stream = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop), "Ncurses Visualizer",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr),
        &stream_events, &data);
    
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = SAMPLE_RATE;
    info.channels = 2;
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(data.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                      params, 1);
    pw_main_loop_run(data.loop);
    pw_stream_destroy(data.stream);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
}

#else // PulseAudio backend
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/channelmap.h>

void audioCaptureThread() {
    pa_sample_spec ss; ss.format = PA_SAMPLE_S16LE; ss.rate = SAMPLE_RATE; ss.channels = 2;
    pa_channel_map map; pa_channel_map_init_stereo(&map);
    pa_buffer_attr buffer_attr; buffer_attr.maxlength = (uint32_t)-1; buffer_attr.fragsize = (uint32_t)(TOTAL_SAMPLES * sizeof(int16_t));
    int error;
    pa_simple *pa = pa_simple_new(nullptr, "Ncurses Visualizer", PA_STREAM_RECORD, nullptr, "record", &ss, &map, &buffer_attr, &error);
    if (!pa) { fprintf(stderr, "PulseAudio error: %s\n", pa_strerror(error)); running = false; return; }
    while (running) {
        int16_t buffer[TOTAL_SAMPLES];
        if (pa_simple_read(pa, buffer, sizeof(buffer), &error) < 0) {
            fprintf(stderr, "PulseAudio read error: %s\n", pa_strerror(error));
            running = false; continue;
        }
        audioBuffer.write(buffer);
    }
    pa_simple_free(pa);
}
#endif

int initColors(const std::vector<std::pair<int, int>>& configPairs, std::pair<int, int> edgeColor) {
    start_color(); use_default_colors(); colorPairIDs.clear();
    for (int i = 0; i < static_cast<int>(configPairs.size()); ++i) {
        init_pair(i + 1, configPairs[i].first, configPairs[i].second);
        colorPairIDs.push_back(i + 1);
    }
    int edgeID = colorPairIDs.size() + 1;
    if (edgeColor.first != -2) { init_pair(edgeID, edgeColor.first, edgeColor.second); }
    return edgeID;
}

int main() {
    ConfigParser parser("oscilloscope.conf");
    std::vector<std::pair<int, int>> colorConfig;
    std::pair<int, int> edgeColorConfig;

    if (!parser.parse()) {
        fprintf(stderr, "Config Error: %s. Using default.\n", parser.getError().c_str());
        colorConfig = { {COLOR_GREEN, -1} }; edgeColorConfig = {-2, -2};
    } else {
        colorConfig = parser.getColorPairs(); edgeColorConfig = parser.getEdgeColorPair();
    }

    std::thread audioThread(audioCaptureThread);
    initscr(); cbreak(); noecho(); curs_set(0); nodelay(stdscr, TRUE); keypad(stdscr, TRUE);
    if (has_colors()) { edgePairID = initColors(colorConfig, edgeColorConfig); }
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);

    using namespace std::chrono;
    const auto frame_duration = microseconds(16667);
    auto next_frame_time = steady_clock::now();
    int16_t leftAudio[BUFFER_FRAMES] = {0};
    int16_t rightAudio[BUFFER_FRAMES] = {0};
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
        int width, height; getmaxyx(stdscr, height, width);
        int draw_height = height - 1;
        if (width < 10 || draw_height < 5) { std::this_thread::sleep_for(milliseconds(100)); continue; }
        erase();
        switch (currentMode) {
            case OSCILLOSCOPE: drawOscilloscope(width, draw_height, leftAudio, rightAudio, colorPairIDs, edgePairID); break;
            case VU_METER: drawVuMeter(width, draw_height, leftAudio, rightAudio, colorPairIDs); break;
            case BAR_GRAPH: drawBarGraph(width, draw_height, leftAudio, rightAudio, colorPairIDs); break;
            default: break;
        }
        attron(A_BOLD); mvprintw(0, 2, "L"); mvprintw(draw_height / 2, 2, "R"); attroff(A_BOLD);
        
        // Simplified FPS counter to reduce main thread load
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
        mvprintw(height - 1, 0, " Mode: %s | Freq: %dHz | FPS: %.0f | Press SPACE to change | Q to quit ", modeNames[currentMode], SAMPLE_RATE, last_fps);
        int curX, curY; getyx(stdscr, curY, curX);
        for(int i = curX; i < width; ++i) addch(' ');
        attroff(A_REVERSE);
        refresh();
        auto sleep_duration = next_frame_time - steady_clock::now();
        if (sleep_duration.count() > 0) std::this_thread::sleep_for(sleep_duration);
    }
    running = false;
    if (audioThread.joinable()) audioThread.join();
    endwin();
    return 0;
}

