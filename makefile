# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2
LDLIBS = -lncurses -lpulse-simple -lpulse

# Project name
TARGET = oscilloscope

# Source files
SRCS = oscilloscope.cpp visualizer.cpp config_parser.cpp
OBJS = $(SRCS:.cpp=.o)

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

# Compile source files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build files
clean:
	rm -f $(TARGET) $(OBJS)

# Install to system (optional)
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

# Uninstall from system (optional)
uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
