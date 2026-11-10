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

# Sin fuentes en src/ todavia no hay biblioteca que enlazar.
ifneq ($(OBJS),)
LIB_DEP := $(LIB)
endif

.PHONY: all test clean
all: $(LIB_DEP) $(TESTS)

$(LIB): $(OBJS)
	ar rcs $@ $^

$(BUILD)/src/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.cpp $(LIB_DEP)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB_DEP) $(LDLIBS) -o $@

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
