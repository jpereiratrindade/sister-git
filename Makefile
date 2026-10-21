CXX ?= g++
CXXFLAGS ?= -std=c++26 -O2 -Wall -Wextra -Wpedantic

.PHONY: all test clean deploy-srv

all: build/sister-git-web

build/sister-git-web: src/sister_git_web.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -lsqlite3

test: all
	bash tests/test_web.sh

deploy-srv: build/sister-git-web
	scp build/sister-git-web jpereiratrindade@10.163.80.129:~/bin/sister-git-web
	scp web/* jpereiratrindade@10.163.80.129:/usr/local/share/sister-git/web/

clean:
	rm -rf build
