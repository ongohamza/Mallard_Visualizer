# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2
SRCS = oscilloscope.cpp visualizer.cpp config_parser.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = visualizer

# --- Audio Backend Selection ---
AUDIO_BACKEND ?= pipewire

ifeq ($(AUDIO_BACKEND),pipewire)
# Use PipeWire with pkg-config for portability
	CXXFLAGS += $(shell pkg-config --cflags libpipewire-0.3)
	CXXFLAGS += -DUSE_PIPEWIRE
	LIBS = -lncurses $(shell pkg-config --libs libpipewire-0.3)
else ifeq ($(AUDIO_BACKEND),pulse)
# Use PulseAudio
	LIBS = -lncurses -lpulse -lpulse-simple
else
	$(error "Invalid AUDIO_BACKEND specified. Use 'pipewire' or 'pulse'.")
endif
# --- End Selection ---

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "Building with $(AUDIO_BACKEND) backend..."
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.cpp config_parser.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
