TARGET := camera-fb
SOURCES := camera-fb.c

CPPFLAGS += -I$(STAGING_INCDIR) -I$(STAGING_INCDIR)/libdrm
LDFLAGS += -L$(STAGING_LIBDIR)
LDLIBS += -ldrm

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET)
