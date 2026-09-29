# PWeb - lightweight C web server (POSIX/Linux/macOS)
# Build: make            -> pweb
# Run:   ./pweb pweb.conf
# Test:  make test

CC     ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDLIBS ?= -lpthread

# Optional TLS client for proxy_pass https:// (defaults off, zero-dep build).
#   make SSL=1   ->  links OpenSSL (-lssl -lcrypto)
ifeq ($(SSL),1)
CFLAGS += -DPWEB_SSL
LDLIBS += -lssl -lcrypto
endif

SRCS   := main.c conn.c config.c http_parser.c handlers.c util.c mime.c log.c
OBJS   := $(SRCS:.c=.o)
TARGET := pweb

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c pweb.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET) pweb.conf

clean:
	rm -f $(OBJS) $(TARGET); rm -f pweb.exe 2>/dev/null || true
