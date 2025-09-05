#include <ncurses.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/channelmap.h>
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
                                                          const std::vector<int>& colorPairIDs, float decay_rate);

const int SAMPLE_RATE = 44100;
const int TOTAL_SAMPLES = BUFFER_FRAMES * 2; // Stereo: 2 samples per frame
std::atomic<bool> running(true);
std::vector<int> colorPairIDs; // Stores actual ncurses color pair IDs
int edgePairID = 0; // Stores edge color pair ID

// Thread-safe audio buffer for stereo
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

        // Deinterleave stereo data
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            destLeft[i] = buffer[i * 2];
            destRight[i] = buffer[i * 2 + 1];
        }

        newData = false; // Mark data as read
        return true;
    }

private:
    int16_t buffer[TOTAL_SAMPLES] = {0};
    std::mutex mutex;
    bool newData = false;
};

AudioBuffer audioBuffer;

void signal_handler(int) {
    running = false;
}

int initColors(const std::vector<std::pair<int, int>>& configPairs,
               std::pair<int, int> edgeColor) {
    start_color();
    use_default_colors();
    colorPairIDs.clear();

    // Initialize regular color pairs
    for (int i = 0; i < static_cast<int>(configPairs.size()); ++i) {
        init_pair(i + 1, configPairs[i].first, configPairs[i].second);
        colorPairIDs.push_back(i + 1);
    }

    // Initialize edge color pair
    int edgeID = colorPairIDs.size() + 1;
    if (edgeColor.first == -2) { // Sentinel value indicating not set
        init_pair(edgeID, COLOR_RED, -1);
    } else {
        init_pair(edgeID, edgeColor.first, edgeColor.second);
    }

    return edgeID;
               }

               void audioCaptureThread() {
                   // Setup PulseAudio for stereo with explicit PipeWire compatibility
                   pa_sample_spec ss;
                   ss.format = PA_SAMPLE_S16LE;
                   ss.rate = SAMPLE_RATE;
                   ss.channels = 2; // Stereo

                   // Create explicit stereo channel map for PipeWire
                   pa_channel_map map;
                   pa_channel_map_init(&map);
                   map.channels = 2;
                   map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
                   map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;

                   pa_buffer_attr buffer_attr;
                   buffer_attr.maxlength = (uint32_t)-1;
                   buffer_attr.tlength = (uint32_t)(TOTAL_SAMPLES * sizeof(int16_t));
                   buffer_attr.prebuf = (uint32_t)-1;
                   buffer_attr.minreq = (uint32_t)-1;
                   buffer_attr.fragsize = (uint32_t)(TOTAL_SAMPLES * sizeof(int16_t));

                   int error;
                   pa_simple *pa = pa_simple_new(
                       nullptr,                   // Use default server
                       "Ncurses Stereo Visualizer (PipeWire)", // Explicit name
                                                 PA_STREAM_RECORD,          // Recording stream
                                                 nullptr,                   // Use default device
                                                 "PipeWire Stereo Visualizer", // Stream description
                                                 &ss,                       // Sample specification
                                                 &map,                      // Explicit stereo channel map
                                                 &buffer_attr,              // Buffer attributes
                                                 &error
                   );

                   if (!pa) {
                       fprintf(stderr, "PipeWire/PulseAudio error: %s\n", pa_strerror(error));
                       running = false;
                       return;
                   }

                   while (running) {
                       int16_t buffer[TOTAL_SAMPLES];
                       if (pa_simple_read(pa, buffer, sizeof(buffer), &error) < 0) {
                           fprintf(stderr, "PipeWire read error: %s\n", pa_strerror(error));
                           running = false;
                           continue;
                       }

                       audioBuffer.write(buffer);
                   }

                   pa_simple_free(pa);
               }

              
              int main() {
                  // Set PipeWire environment variables for proper stereo recognition
                  setenv("PULSE_PROP_application.id", "com.github.ncurses_visualizer", 1);
                  setenv("PULSE_PROP_media.role", "music", 1);
                  setenv("PULSE_PROP_application.name", "NCurses Stereo Visualizer", 1);

                  // Parse configuration
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

                  // Start audio capture thread
                  std::thread audioThread(audioCaptureThread);

                  // Setup ncurses
                  initscr();
                  cbreak();
                  noecho();
                  curs_set(0);
                  nodelay(stdscr, TRUE);
                  keypad(stdscr, TRUE);

                  // Initialize colors from config
                  if (has_colors()) {
                      edgePairID = initColors(colorConfig, edgeColorConfig);
                  } else {
                      fprintf(stderr, "Terminal does not support colors\n");
                  }

                  // Signal handling
                  signal(SIGINT, signal_handler);
                  signal(SIGTERM, signal_handler);

                  // Timing control
                  using namespace std::chrono;
                  const auto frame_duration = microseconds(16667); // 60 FPS
                  auto next_frame_time = steady_clock::now();

                  // Audio data buffers
                  int16_t leftAudio[BUFFER_FRAMES] = {0};
                  int16_t rightAudio[BUFFER_FRAMES] = {0};
                  int total_frames = 0;
                  auto last_info_update_time = steady_clock::now();
                  float last_fps = 0.0f;

                  // Visualization mode
                  VisualizationMode currentMode = OSCILLOSCOPE;
                  const char* modeNames[] = {"Oscilloscope", "VU Meter", "Bar Graph"};

                  while (running) {
                      next_frame_time += frame_duration;

                      if (!audioBuffer.read(leftAudio, rightAudio)) {
                          // No new audio data, skip visualization
                          std::this_thread::sleep_for(milliseconds(1));
                          continue;
                      }

                      int ch = getch();
                      if (ch != ERR) {
                          if (ch == 'q' || ch == 'Q') {
                              running = false;
                          } else if (ch == ' ') {
                              // Cycle through visualization modes
                              currentMode = static_cast<VisualizationMode>(
                                  (static_cast<int>(currentMode) + 1) % NUM_MODES);
                          }
                      }

                      int width, height;
                      getmaxyx(stdscr, height, width);
                      int draw_height = height - 1; // Reserve last row for status

                      if (width < 10 || draw_height < 5) {
                          std::this_thread::sleep_for(milliseconds(100));
                          continue;
                      }

                      erase();

                      // Draw selected visualization
                      switch (currentMode) {
                          case OSCILLOSCOPE:
                              drawOscilloscope(width, draw_height, leftAudio, rightAudio,
                                               colorPairIDs, edgePairID);
                              break;
                          case VU_METER:
                              drawVuMeter(width, draw_height, leftAudio, rightAudio,
                                          colorPairIDs);
                              break;
                          case BAR_GRAPH:
                              drawBarGraph(width, draw_height, leftAudio, rightAudio,
                                           colorPairIDs);
                              break;
                      }

                      // Add channel labels
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

                      // Update status bar
                      attron(A_REVERSE);
                      mvprintw(height - 1, 0, " Mode: %s | Freq: %dHz | FPS: %.1f | Press SPACE to change | Q to quit ",
                               modeNames[currentMode], SAMPLE_RATE, last_fps);
                      // Fill remaining space in status bar
                      int curX, curY;
                      getyx(stdscr, curY, curX);
                      for(int i = curX; i < width; ++i) {
                          addch(' ');
                      }
                      attroff(A_REVERSE);

                      refresh();

                      auto sleep_duration = next_frame_time - steady_clock::now();
                      if (sleep_duration.count() > 0) {
                          std::this_thread::sleep_for(sleep_duration);
                      }
                  }

                  running = false;
                  if(audioThread.joinable()) {
                      audioThread.join();
                  }
                  endwin();
                  return 0;
              }
