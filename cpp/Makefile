CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread -static-libstdc++ -static-libgcc

SRCS := $(filter-out src/test_main.cpp $(wildcard src/test_*.cpp),$(wildcard src/*.cpp)) src/gen/embedded_proto.cpp src/gen/x11_tables.cpp src/gen/x11_err_evt.cpp
OBJS := $(SRCS:.cpp=.o)
BIN := build/wayland-track

TEST_SRCS := $(wildcard src/test_*.cpp) $(filter-out src/main.cpp,$(SRCS))
TEST_OBJS := $(TEST_SRCS:.cpp=.o)
TESTBIN := build/wayland-track-test

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(TESTBIN): $(TEST_OBJS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

gen:
	python3 tools/gen_embedded_proto.py
	python3 tools/gen_x11.py

test: $(TESTBIN)
	$(TESTBIN)

clean:
	rm -rf build $(OBJS)

.PHONY: all test gen clean
