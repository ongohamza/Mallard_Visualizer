#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H
#include <ncurses.h>
#include <string>
#include <vector>
#include <utility>

const int BUFFER_FRAMES = 256;

enum class ShapeVisualizerType {
    EXPAND,
    DISTORT
};

struct CustomVisualizer {
    std::string name;
    ShapeVisualizerType type = ShapeVisualizerType::EXPAND;
    std::vector<std::vector<std::pair<float, float>>> polygons;
};

class ConfigParser {
public:
    explicit ConfigParser(const std::string& filename);
    bool parse();
    std::vector<std::pair<int, int>> getColorPairs() const;
    std::string getError() const;
    std::vector<CustomVisualizer> getCustomVisualizers() const;

private:
    std::string filename;
    std::vector<std::pair<int, int>> gradientColorPairs;
    std::string error;
    std::vector<CustomVisualizer> customVisualizers;
    int parseColor(const std::string& colorStr);
};

#endif // CONFIG_PARSER_H

extern float decay__factor;
