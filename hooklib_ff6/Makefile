PKG_CONFIG ?= pkg-config

CXX := g++
CXXFLAGS := -g -std=c++20 -Wall -Wextra -O2 -fPIC -I../snes -I../zip -I../mem_viewer
LDFLAGS := -shared
TARGET := libhooklib_ff6.so
SOURCES := ff6_hook.cpp
OBJECTS := $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
