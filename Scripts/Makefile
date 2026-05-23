# RMG NAT traversal + index server (UDP 6364, HTTP index 6365)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = nat_traversal_server
SRC = nat_traversal_server.c

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TARGET).exe
