CC ?= cc
BUILD := build
RAYLIB_SRC := engine/raylib/src
CORE_SRC := $(wildcard engine/core/*.c)
CORE_OBJ := $(patsubst engine/core/%.c,$(BUILD)/core_%.o,$(CORE_SRC))
RAYLIB_LIB := $(RAYLIB_SRC)/libraylib.a
CFLAGS := -std=c99 -O2 -g -Wall -Wextra -Wpedantic -DGRAPHICS_API_OPENGL_21 -DSUPPORT_FILEFORMAT_BDF
LDLIBS := -lm -lpthread -ldl -lGL -lX11

.PHONY: all run smoke clean

all: $(BUILD)/ui_tool

$(RAYLIB_LIB):
	$(MAKE) -C $(RAYLIB_SRC) GRAPHICS=GRAPHICS_API_OPENGL_21 CUSTOM_CFLAGS='-DSUPPORT_FILEFORMAT_BDF' -j4

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/core

$(BUILD)/core_%.o: engine/core/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Iengine -I$(RAYLIB_SRC) -MMD -MP -c $< -o $@

$(BUILD)/ui_tool.o: ui_tool/main.c | $(BUILD)
	$(CC) $(CFLAGS) -I. -I$(RAYLIB_SRC) -MMD -MP -c $< -o $@

$(BUILD)/ui_tool: $(BUILD)/ui_tool.o $(CORE_OBJ) $(RAYLIB_LIB)
	$(CC) $^ $(LDLIBS) -o $@

run: $(BUILD)/ui_tool
	./$(BUILD)/ui_tool

smoke: $(BUILD)/ui_tool
	./$(BUILD)/ui_tool --smoke

clean:
	rm -rf $(BUILD)

-include $(CORE_OBJ:.o=.d) $(BUILD)/ui_tool.d

