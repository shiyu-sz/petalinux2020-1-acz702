TARGET := lvgl-mp4-demo
SOURCES := lvgl-mp4-demo.c

CPPFLAGS += -I$(STAGING_INCDIR) -I$(STAGING_INCDIR)/libdrm \
	-I$(STAGING_INCDIR)/lvgl \
	-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
LDFLAGS += -L$(STAGING_LIBDIR)
LDLIBS += -llvgl -lavformat -lavcodec -lavutil -lswscale -ldrm -lm

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET)
