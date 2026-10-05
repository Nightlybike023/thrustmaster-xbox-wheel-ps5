PS5_HOST ?= 192.168.15.50
PORT     ?= 9021
TARGET   := ps5-wheel-driver.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -g -O2 -fPIC -fno-stack-protector
LDFLAGS += -lScePad -lSceUserService -lpthread -ldl

SRC := gc_main.c shellui_pad.c usb_helpers.c controller_nintendo.c controller_xbox.c controller_wheel.c web_server.c instance_guard.c
HEADERS := $(wildcard *.h) web_ui.h

.PHONY: all clean deploy

all: $(TARGET)

web_ui.h: web/config.html web/overlay.html embed_web.py
	python3 embed_web.py

$(TARGET): $(SRC) $(HEADERS) Makefile
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

deploy: $(TARGET)
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
