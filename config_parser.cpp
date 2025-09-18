#include "config_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <map>

ConfigParser::ConfigParser(const std::string& filename)
: filename(filename), edgeColorPair(-2, -2) {} // Sentinel for not set

bool ConfigParser::parse() {
    std::ifstream file(filename);
    if (!file.is_open()) {
        error = "Failed to open config file: " + filename;
        return false;
    }

    std::string line;
    int lineNum = 0;
    while (std::getline(file, line)) {
        lineNum++;
        // Trim whitespace
        line.erase(line.find_last_not_of(" \t") + 1);
        line.erase(0, line.find_first_not_of(" \t"));

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Split into key/value
        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            std::ostringstream oss;
            oss << "Syntax error line " << lineNum << ": Missing '='";
            error = oss.str();
            return false;
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // Trim key/value
        key.erase(key.find_last_not_of(" \t") + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));

        // Handle gradient color configuration
        if (key == "gradient_color") {
            size_t commaPos = value.find(',');
            if (commaPos == std::string::npos) {
                std::ostringstream oss;
                oss << "Syntax error line " << lineNum << ": Expected 'fg,bg'";
                error = oss.str();
                return false;
            }

            std::string fgStr = value.substr(0, commaPos);
            std::string bgStr = value.substr(commaPos + 1);
            int fg = parseColor(fgStr);
            int bg = parseColor(bgStr);

            if (fg == -1 || bg == -1) {
                std::ostringstream oss;
                oss << "Invalid color at line " << lineNum
                << ". Valid colors: black, red, green, yellow, blue, magenta, cyan, white";
                error = oss.str();
                return false;
            }

            gradientColorPairs.emplace_back(fg, bg);
        } else if (key == "edge_color") {
            size_t commaPos = value.find(',');
            if (commaPos == std::string::npos) {
                std::ostringstream oss;
                oss << "Syntax error line " << lineNum << ": Expected 'fg,bg' for edge_color";
                error = oss.str();
                return false;
            }

            std::string fgStr = value.substr(0, commaPos);
            std::string bgStr = value.substr(commaPos + 1);
            int fg = parseColor(fgStr);
            int bg = parseColor(bgStr);

            if (fg == -1 || bg == -1) {
                std::ostringstream oss;
                oss << "Invalid edge color at line " << lineNum
                << ". Valid colors: black, red, green, yellow, blue, magenta, cyan, white";
                error = oss.str();
                return false;
            }

            edgeColorPair = {fg, bg};
        }
    }

    if (gradientColorPairs.empty()) {
        error = "No gradient_color configurations found";
        return false;
    }

    return true;
}

std::vector<std::pair<int, int>> ConfigParser::getColorPairs() const {
    return gradientColorPairs;
}

std::pair<int, int> ConfigParser::getEdgeColorPair() const {
    return edgeColorPair;
}

std::string ConfigParser::getError() const {
    return error;
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

