PLUGIN_NAME=dynamic-cursors

SOURCE_FILES := $(wildcard ./src/*.cpp ./src/*/*.cpp ./src/*/*/*.cpp)
HEADER_FILES := $(wildcard ./src/*.hpp ./src/*/*.hpp ./src/*/*/*.hpp)

OBJECT_FILES := $(patsubst ./src/%.cpp, out/%.o, $(SOURCE_FILES))
DEPENDENCY_FILES := $(OBJECT_FILES:.o=.d)
CXXFLAGS := -Wall -fPIC -std=c++26 -g \
	$(shell pkg-config --cflags hyprland pixman-1 libdrm | sed 's#-I\([^ ]*/hyprland\)\($$\| \)#-I\1 -I\1/src #g')

OUTPUT=out/$(PLUGIN_NAME).so

ifeq ($(CXX),g++)
    CXXFLAGS += --no-gnu-unique
endif

.PHONY: all clean load unload format test

all: $(OUTPUT)

$(OUTPUT): $(OBJECT_FILES)
	$(CXX) -shared $^ $(LDFLAGS) -o $@

# Compile step (object file per .cpp)
out/%.o: ./src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_CXXFLAGS) -MMD -MP -c $< -o $@

# Objects compiled before dependency tracking have no .d file, so make would
# keep them even when a header changed an embedded class layout (the ODR
# mismatch behind the luaL_alloc abort). Anchoring every object to the
# Makefile forces one full rebuild the first time this Makefile runs;
# afterwards the -MMD files track headers exactly.
$(OBJECT_FILES): Makefile

clean:
	$(RM) $(OUTPUT) $(OBJECT_FILES) $(DEPENDENCY_FILES) out/TrailTest out/SpringTest

format:
	clang-format -i $(HEADER_FILES) $(SOURCE_FILES)

out/TrailTest: tests/TrailTest.cpp src/other/Trail.cpp src/other/Trail.hpp
	@mkdir -p $(dir $@)
	$(CXX) -Wall -Wextra -std=c++26 tests/TrailTest.cpp src/other/Trail.cpp -lhyprutils -o $@

out/SpringTest: tests/SpringTest.cpp src/other/Spring.cpp src/other/Spring.hpp
	@mkdir -p $(dir $@)
	$(CXX) -Wall -Wextra -std=c++26 tests/SpringTest.cpp src/other/Spring.cpp -o $@

test: out/TrailTest out/SpringTest
	./out/TrailTest
	./out/SpringTest

load: all unload
	hyprctl plugin load ${PWD}/$(OUTPUT)

unload:
	hyprctl plugin unload ${PWD}/$(OUTPUT)

-include $(DEPENDENCY_FILES)
