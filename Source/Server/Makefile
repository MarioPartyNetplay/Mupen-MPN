# RMG NAT traversal + index server (UDP 9150, HTTP index 9151)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = nat_traversal_server
SRC = nat_traversal_server.c

ifeq ($(wildcard turn_secrets.h),turn_secrets.h)
CFLAGS += -DHAS_TURN_SECRETS_H
else
$(warning turn_secrets.h not found — copy turn_secrets.h.example to turn_secrets.h to embed Cloudflare TURN credentials)
endif

# TURN broker requires libcurl (HTTPS to Cloudflare).
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(CURL_LIBS),)
$(error libcurl is required for /turn/ice-servers — install libcurl-dev (e.g. pacman -S curl))
endif
CFLAGS += $(CURL_CFLAGS) -DWITH_LIBCURL
LDFLAGS += $(CURL_LIBS)

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TARGET).exe
