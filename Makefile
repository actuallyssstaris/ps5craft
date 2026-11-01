# PS5Craft - build
#
# Host build: pure C++ shared backend + tests. No PS5 SDK needed.
# PS5 build: prospero-clang18 + prospero-lld + native_app_builder (see tools/).
#
#   make host        # build host test binary
#   make test        # run host tests
#   make app         # placeholder: builds the PS5 title (needs SDK toolchain)

CXX ?= c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Icore -Iplatform/host
LDFLAGS :=

HOST_SRCS := \
  core/json.cpp \
  core/fs.cpp \
  core/instance.cpp \
  core/server.cpp \
  core/backend.cpp \
  platform/process_host.cpp \
  tests/test_main.cpp \
  tests/http_host.cpp

HOST_OBJS := $(patsubst %.cpp,build/host/%.o,$(HOST_SRCS))

.PHONY: all host test clean app

all: host

host: build/host/ps5craft_test build/host/ps5craft_http

build/host/ps5craft_http: tests/http_host.cpp $(filter-out tests/test_main.cpp,$(HOST_OBJS))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) tests/http_host.cpp $(filter-out tests/test_main.cpp,$(HOST_OBJS)) -o $@ $(LDFLAGS)

build/host/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/host/ps5craft_test: $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(HOST_OBJS) -o $@ $(LDFLAGS)

test: build/host/ps5craft_test
	./build/host/ps5craft_test

clean:
	rm -rf build dist

app:
	@echo "PS5 app build not wired yet: needs PS5_PAYLOAD_SDK and the pinned"
	@echo "ps5platform/RADV dependencies. See docs/research.md."
	@exit 1
