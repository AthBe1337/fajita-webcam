CXX      ?= g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pthread
LDFLAGS  := -ljpeg -pthread

# NEON is default on aarch64, but be explicit for potential cross-compile
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),aarch64)
  CXXFLAGS += -march=armv8-a+simd
endif

SRC_DIR := src
SRCS    := $(filter-out $(SRC_DIR)/json.hpp, $(wildcard $(SRC_DIR)/*.cpp))
OBJS    := $(SRCS:.cpp=.o)
TARGET  := fajita-webcam

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# main.cpp includes json.hpp which is large; avoid recompiling everything for it
$(SRC_DIR)/main.o: $(SRC_DIR)/main.cpp $(SRC_DIR)/json.hpp $(wildcard $(SRC_DIR)/*.h)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)
