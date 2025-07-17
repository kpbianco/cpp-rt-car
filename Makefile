# Project Name
TARGET = simcore

# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -Wpedantic -march=native

# Source files
SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:.cpp=.o)

# Output directory (optional)
BIN_DIR = bin
OBJ_DIR = obj

# Default rule
all: $(BIN_DIR)/$(TARGET)

# Compile and link
$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Ensure bin/ exists
$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Clean
clean:
	rm -f *.o
	rm -rf $(BIN_DIR)

# Phony targets
.PHONY: all clean
