#include "config_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <map>
#include <cmath>

float decay__factor = 0.025f;
ConfigParser::ConfigParser(const std::string& filename)
: filename(filename) {}

std::vector<CustomVisualizer> ConfigParser::getCustomVisualizers() const {
    return customVisualizers;
}

std::vector<std::pair<int, int>> ConfigParser::getColorPairs() const {
    return gradientColorPairs;
}

std::string ConfigParser::getError() const {
    return error;
}

bool ConfigParser::parse() {
    std::ifstream file(filename);
    if (!file.is_open()) {
        error = "Failed to open config file: " + filename;
        return false;
    }

    std::string line;
    int lineNum = 0;
    bool parsing_visualizer = false;
    CustomVisualizer current_visualizer;

    while (std::getline(file, line)) {
        lineNum++;
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        if (line.empty() || line[0] == '#') {
            if (parsing_visualizer && !current_visualizer.polygons.empty() && !current_visualizer.polygons.back().empty()) {
                current_visualizer.polygons.push_back({});
            }
            continue;
        }

        if (parsing_visualizer) {
            if (line == "}") {
                if (!current_visualizer.polygons.empty() && current_visualizer.polygons.back().empty()) {
                    current_visualizer.polygons.pop_back();
                }
                customVisualizers.push_back(current_visualizer);
                parsing_visualizer = false;
                continue;
            }

            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            key.erase(key.find_last_not_of(" \t") + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));

            if (key == "point") {
                size_t commaPos = value.find(',');
                if (commaPos == std::string::npos) continue;
                try {
                    float x = std::stof(value.substr(0, commaPos));
                    float y = std::stof(value.substr(commaPos + 1));
                    if (current_visualizer.polygons.empty()) {
                        current_visualizer.polygons.push_back({});
                    }
                    current_visualizer.polygons.back().push_back({x, y});
                } catch (const std::exception&) { continue; }
            } else if (key == "shape" && value == "circle") {
                int points = 128;
                std::streampos old_pos = file.tellg();
                if (std::getline(file, line)) {
                    line.erase(0, line.find_first_not_of(" \t"));
                    line.erase(line.find_last_not_of(" \t") + 1);
                    if (line.rfind("points", 0) == 0) {
                        size_t p_pos = line.find('=');
                        if (p_pos != std::string::npos) {
                           try { points = std::stoi(line.substr(p_pos + 1)); }
                           catch(const std::exception&) {}
                        }
                    } else {
                       file.seekg(old_pos);
                    }
                }
                std::vector<std::pair<float, float>> circle_poly;
                const float PI = 3.1415926535f;
                for(int i = 0; i < points; ++i) {
                    float angle = 2.0f * PI * i / points;
                    circle_poly.push_back({100.0f * std::cos(angle), 100.0f * std::sin(angle)});
                }
                current_visualizer.polygons.push_back(circle_poly);
            } else if (key == "visualizer_type") {
                if (value == "distort") {
                    current_visualizer.type = ShapeVisualizerType::DISTORT;
                } else {
                    current_visualizer.type = ShapeVisualizerType::EXPAND;
                }
            }
        } else {
            size_t spacePos = line.find(' ');
            if (spacePos != std::string::npos) {
                std::string command = line.substr(0, spacePos);
                if (command == "new_visualizer") {
                    size_t bracePos = line.find('{');
                    if (bracePos == std::string::npos) continue;
                    parsing_visualizer = true;
                    current_visualizer = CustomVisualizer();
                    current_visualizer.name = line.substr(spacePos + 1, bracePos - spacePos - 2);
                    current_visualizer.polygons.clear();
                    continue;
                }
            }
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));

                if (key == "gradient_color") {
                    size_t commaPos = value.find(',');
                    if (commaPos == std::string::npos) continue;
                    int fg = parseColor(value.substr(0, commaPos));
                    int bg = parseColor(value.substr(commaPos + 1));
                    if(fg != -1 && bg != -1) gradientColorPairs.emplace_back(fg,bg);
                } else if (key == "visualizer_decay_factor") {
                     try { decay__factor = std::stof(value); }
                     catch (const std::exception&) { continue; }
                }
            }
        }
    }
    if (gradientColorPairs.empty()) {
        gradientColorPairs.emplace_back(COLOR_GREEN, -1);
    }
    return true;
}

int ConfigParser::parseColor(const std::string& colorStr) {
    static const std::map<std::string, int> colorMap = {
        {"black", COLOR_BLACK}, {"red", COLOR_RED},
        {"green", COLOR_GREEN}, {"yellow", COLOR_YELLOW},
        {"blue", COLOR_BLUE}, {"magenta", COLOR_MAGENTA},
        {"cyan", COLOR_CYAN}, {"white", COLOR_WHITE}
    };
    auto it = colorMap.find(colorStr);
    return (it != colorMap.end()) ? it->second : -1;
}
