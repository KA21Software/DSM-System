CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread
CPPFLAGS += -Iinclude -MMD -MP
LDLIBS   += -pthread

BUILD := build
LIB   := $(BUILD)/libdsm.a

SRCS      := $(wildcard src/*.cpp)
OBJS      := $(SRCS:src/%.cpp=$(BUILD)/src/%.o)
TEST_SRCS := $(wildcard tests/test_*.cpp)
TESTS     := $(TEST_SRCS:tests/%.cpp=$(BUILD)/tests/%)
APP_SRCS  := $(wildcard apps/*.cpp)
APPS      := $(APP_SRCS:apps/%.cpp=$(BUILD)/apps/%)

# Sin fuentes en src/ todavia no hay biblioteca que enlazar.
ifneq ($(OBJS),)
LIB_DEP := $(LIB)
endif

.PHONY: all test itest clean demo
all: $(LIB_DEP) $(TESTS) $(APPS)

$(LIB): $(OBJS)
	ar rcs $@ $^

$(BUILD)/src/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.cpp $(LIB_DEP)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB_DEP) $(LDLIBS) -o $@

$(BUILD)/apps/%: apps/%.cpp $(LIB_DEP)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB_DEP) $(LDLIBS) -o $@

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

# Integration tests: several processes of this PC acting as nodes (a few minutes).
itest: all
	tests/test_cluster.sh 3
	tests/test_cluster.sh 4
	tests/test_apps.sh 3 256 6
	tests/test_failure.sh

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

demo: all
	tests/demo_local.sh
