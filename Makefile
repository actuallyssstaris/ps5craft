# PS5Craft - build
#
# Host build: pure C++ shared backend + tests. No PS5 SDK needed.
# PS5 build: prospero-clang18 + prospero-lld + native_app_builder (see tools/).
#
#   make host        # build host test + HTTP harness binaries
#   make test        # run host unit tests
#   make app         # build + sign the PS5 title into dist/PPSA90104/

CXX ?= c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Icore -Iplatform/host
LDFLAGS :=

COMMON_SRCS := \
  core/json.cpp \
  core/fs.cpp \
  core/zip.cpp \
  core/backup.cpp \
  core/instance.cpp \
  core/server.cpp \
  core/backend.cpp \
  platform/process_host.cpp

COMMON_OBJS := $(patsubst %.cpp,build/host/%.o,$(COMMON_SRCS))
TEST_OBJS := build/host/tests/test_main.o
HTTP_OBJS := build/host/tests/http_host.o
APP_OBJS := build/host/src/main.o build/host/src/http_api.o

.PHONY: all host test clean app

all: host

host: build/host/ps5craft_test build/host/ps5craft_http build/host/ps5craft_host

build/host/ps5craft_host: $(COMMON_OBJS) $(APP_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(COMMON_OBJS) $(APP_OBJS) -o $@ -lmicrohttpd -lpthread

build/host/ps5craft_http: $(COMMON_OBJS) $(HTTP_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(COMMON_OBJS) $(HTTP_OBJS) -o $@ $(LDFLAGS)

build/host/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/host/ps5craft_test: $(COMMON_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(COMMON_OBJS) $(TEST_OBJS) -o $@ $(LDFLAGS)

test: build/host/ps5craft_test
	./build/host/ps5craft_test

clean:
	rm -rf build dist

app:
	PS5_PAYLOAD_SDK=$${PS5_PAYLOAD_SDK:-/root/projects/PlaySurf/.deps/native/ps5-payload-sdk} \
	  tools/build_ps5.sh
