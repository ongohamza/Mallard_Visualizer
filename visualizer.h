#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <ncurses.h>
#include <cstdint>
#include <vector>
#include "config_parser.h"

void drawOscilloscope(WINDOW *win, int width, int height,
                      const int16_t* leftData, const int16_t* rightData,
                      const std::vector<int>& colorPairIDs, int edgePairID);

void drawVuMeter(WINDOW *win, int width, int height,
                 const int16_t* leftData, const int16_t* rightData,
                 const std::vector<int>& colorPairIDs, bool audio_active);

void drawBarGraph(WINDOW *win, int width, int height,
                  const int16_t* leftData, const int16_t* rightData,
                  const std::vector<int>& colorPairIDs, bool audio_active);

void drawGalaxy(WINDOW *win, int width, int height,
                const int16_t* leftData, const int16_t* rightData,
                const std::vector<int>& colorPairIDs, bool audio_active);

// Added hardcoded Ellipse and Eclipse back
void drawEllipse(WINDOW *win, int width, int height,
                   const int16_t* leftData, const int16_t* rightData,
                   const std::vector<int>& colorPairIDs);

void drawEclipse(WINDOW *win, int width, int height,
                 const int16_t* leftData, const int16_t* rightData,
                 const std::vector<int>& colorPairIDs);

// Generic function for config-defined shapes
void drawCustomShape(WINDOW *win, int width, int height,
                     const int16_t* leftData, const int16_t* rightData,
                     const std::vector<int>& colorPairIDs,
                     const CustomVisualizer& visualizer);

void toggleVuMeterMode(bool upArrow);
const char* getVuMeterModeName();

#endif // VISUALIZER_H
