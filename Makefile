CXX ?= g++
CXXFLAGS ?= -std=c++26 -O2 -Wall -Wextra -Wpedantic

.PHONY: all test clean

all: build/sister-git-web

build/sister-git-web: src/sister_git_web.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

test: all
	bash tests/test_web.sh

clean:
	rm -rf build
