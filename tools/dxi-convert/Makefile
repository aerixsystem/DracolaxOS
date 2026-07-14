# tools/dxi-convert/Makefile
# Builds the host-side .dxi converter (runs on Linux/macOS, not in the kernel)
#
# Prerequisites:
#   stb_image.h and stb_image_resize2.h must be in this directory.
#   Download once:
#     curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
#     curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h
#
# Build:   make
# Clean:   make clean
# Install: make install   (copies to /usr/local/bin)

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11
LDFLAGS := -lm
TARGET  := dxi_convert

.PHONY: all clean install deps

all: deps $(TARGET)

$(TARGET): dxi_convert.c stb_image.h
	$(CC) $(CFLAGS) -o $@ dxi_convert.c $(LDFLAGS)
	@echo "Built: $@"

deps:
	@if [ ! -f stb_image.h ]; then \
		echo "Downloading stb_image.h..."; \
		curl -fsSL -O https://raw.githubusercontent.com/nothings/stb/master/stb_image.h || \
		wget -q https://raw.githubusercontent.com/nothings/stb/master/stb_image.h; \
	fi
	@if [ ! -f stb_image_resize2.h ]; then \
		echo "Downloading stb_image_resize2.h..."; \
		curl -fsSL -O https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h || \
		wget -q https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h; \
	fi

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed to /usr/local/bin/$(TARGET)"

clean:
	rm -f $(TARGET)
