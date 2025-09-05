
# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2
SRCS = oscilloscope.cpp visualizer.cpp config_parser.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = visualizer

# --- Audio Backend Selection ---
# User can override this from the command line, e.g., 'make AUDIO_BACKEND=pulse'
# Defaults to pipewire if not set.
AUDIO_BACKEND ?= pipewire

# Check the value of AUDIO_BACKEND and set flags/libraries accordingly
ifeq ($(AUDIO_BACKEND),pipewire)
    # Use PipeWire
	@echo "Building with PipeWire backend..."
    CXXFLAGS += -DUSE_PIPEWIRE
    LIBS = -lncurses -lpipewire-0.3
else ifeq ($(AUDIO_BACKEND),pulse)
    # Use PulseAudio
	@echo "Building with PulseAudio backend..."
    # No specific flag needed, it's the default in the C++ code
    LIBS = -lncurses -lpulse-simple
else
    # Error for invalid backend
    $(error "Invalid AUDIO_BACKEND specified. Use 'pipewire' or 'pulse'.")
endif
# --- End Selection ---

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.cpp config_parser.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
