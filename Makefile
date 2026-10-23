CXX ?= g++
CXXFLAGS ?= -std=c++26 -O2 -Wall -Wextra -Wpedantic
VERSION := $(shell tr -d '[:space:]' < VERSION)
CPPFLAGS += -DSISTER_GIT_VERSION='"$(VERSION)"'

.PHONY: all test test-e2e clean deploy-srv

all: build/sister-git-web

build/sister-git-web: src/sister_git_web.cpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ -lsqlite3

test: all
	bash tests/test_security.sh
	bash tests/test_web.sh

test-e2e: all
	bash tests/test_browser_e2e.sh

deploy-srv: build/sister-git-web
	scp build/sister-git-web jpereiratrindade@10.163.80.129:~/bin/sister-git-web.new
	ssh -t jpereiratrindade@10.163.80.129 'sudo install -o root -g root -m 0755 ~/bin/sister-git-web.new /usr/local/bin/sister-git-web && rm ~/bin/sister-git-web.new && sudo systemctl restart sister-git-web.service'
	scp web/* jpereiratrindade@10.163.80.129:/usr/local/share/sister-git/web/
	ssh jpereiratrindade@10.163.80.129 'systemctl is-active --quiet sister-git-web.service && curl -fsS http://127.0.0.1:8787/api/v1/health'

clean:
	rm -rf build
