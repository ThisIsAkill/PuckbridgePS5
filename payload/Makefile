PS5_HOST ?= 192.168.69.44
PORT     ?= 9021
TARGET   := ghost-control-ps5.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -g -O2 -fPIC -fno-stack-protector
LDFLAGS += -lScePad -lSceUserService -lSceSystemService -lpthread -ldl

SRC := gc_main.c shellui_pad.c usb_helpers.c controller_nintendo.c controller_xbox.c controller_steam.c controller_sc2.c sc2_profile.c game_list.c webui.c

.PHONY: all clean deploy

all: $(TARGET)

webui_html.h: web/index.html gen_html.py
	python3 gen_html.py

$(TARGET): $(SRC) webui_html.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

deploy: $(TARGET)
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
