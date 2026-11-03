LVGL_DIR ?= $(CURDIR)/lvgl-8.3.11
BUILD_DIR ?= $(CURDIR)/.lvgl-build

SOURCES := $(shell find $(LVGL_DIR)/src -type f -name '*.c' | sort)
OBJECTS := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

CPPFLAGS += -I$(CURDIR) -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
CFLAGS += -std=gnu99

.PHONY: all clean
all: liblvgl.a

liblvgl.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(LVGL_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) liblvgl.a
