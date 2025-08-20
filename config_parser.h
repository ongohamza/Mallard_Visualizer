
#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H
#include <ncurses.h>
#include <string>
#include <vector>
#include <utility>

const int BUFFER_FRAMES = 256; // This is a global variable for that is used in both visualizer.cpp and oscilloscope.cpp has nothing to do with parsing the config file

class ConfigParser {
public:
    explicit ConfigParser(const std::string& filename);
    bool parse();
    // This will now be used for the gradient colors
    std::vector<std::pair<int, int>> getColorPairs() const;
    std::pair<int, int> getEdgeColorPair() const;
    std::string getError() const;

private:
    std::string filename;
    // Renamed for clarity: this vector will now store the gradient.
    std::vector<std::pair<int, int>> gradientColorPairs;
    std::pair<int, int> edgeColorPair;
    std::string error;
    int parseColor(const std::string& colorStr);
};

#endif // CONFIG_PARSER_H
