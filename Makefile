# PS5Craft - build
#
# Host build: pure C++ shared backend + tests. No PS5 SDK needed.
# PS5 build: prospero-clang18 + prospero-lld + native_app_builder (see tools/).
#
#   make host        # build host test + HTTP harness binaries
#   make test        # run host unit tests
#   make app         # build + sign the PS5 title into dist/PPSA90104/

CXX ?= c++
DEPFLAGS := -MMD -MP
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Icore -Iplatform/host $(DEPFLAGS)
LDFLAGS :=

COMMON_SRCS := \
  core/json.cpp \
  core/hash.cpp \
  core/http_client.cpp \
  core/monitor.cpp \
  core/catalog.cpp \
  core/fs.cpp \
  core/zip.cpp \
  core/backup.cpp \
  core/instance.cpp \
  core/server.cpp \
  core/backend.cpp \
  core/modstore.cpp \
  core/storage.cpp \
  core/trace.cpp \
  platform/process_host.cpp

COMMON_OBJS := $(patsubst %.cpp,build/host/%.o,$(COMMON_SRCS))
TEST_OBJS := build/host/tests/test_main.o
HTTP_OBJS := build/host/tests/http_host.o
APP_OBJS := build/host/src/main.o build/host/src/http_api.o

# Every object records the headers it used, so a struct change rebuilds the
# files that include it.
-include $(COMMON_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(HTTP_OBJS:.o=.d) \
         $(APP_OBJS:.o=.d)

.PHONY: all host test clean app

all: host

host: build/host/ps5craft_test build/host/ps5craft_http build/host/ps5craft_host

build/host/ps5craft_host: $(COMMON_OBJS) $(APP_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(COMMON_OBJS) $(APP_OBJS) -o $@ -lmicrohttpd -lcurl -lpthread

build/host/ps5craft_http: $(COMMON_OBJS) $(HTTP_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(COMMON_OBJS) $(HTTP_OBJS) -o $@ $(LDFLAGS) -lcurl

build/host/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/host/ps5craft_test: $(COMMON_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(COMMON_OBJS) $(TEST_OBJS) -o $@ $(LDFLAGS) -lcurl

test: build/host/ps5craft_test
	./build/host/ps5craft_test

clean:
	rm -rf build dist

app:
	bash gui/tools/build-kit.sh app TITLE_ID=PPSA90104 APP_NAME=PS5Craft
