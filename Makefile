TARGET = simcore

CXX = g++
BASEFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -pthread -march=native
RELFLAGS = -O3
DBGFLAGS = -O0 -g

LOGFLAGS_INFO  = -DLOG_ENABLED -DLOG_DEFAULT_LEVEL=2
LOGFLAGS_TRACE = -DLOG_ENABLED -DLOG_DEFAULT_LEVEL=0
PROFFLAG = -DPROF_ENABLED

SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:.cpp=.o)
BIN_DIR = bin

all: CXXFLAGS = $(BASEFLAGS) $(RELFLAGS)
all: $(BIN_DIR)/$(TARGET)

debug: CXXFLAGS = $(BASEFLAGS) $(DBGFLAGS) $(LOGFLAGS_INFO) $(PROFFLAG)
debug: clean $(BIN_DIR)/$(TARGET)

trace: CXXFLAGS = $(BASEFLAGS) $(DBGFLAGS) $(LOGFLAGS_TRACE) $(PROFFLAG)
trace: clean $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o
	rm -rf $(BIN_DIR)

.PHONY: all clean debug trace
