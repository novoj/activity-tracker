CC = gcc
PKG_CONFIG ?= pkg-config
CFLAGS = $(shell $(PKG_CONFIG) --cflags gio-2.0 gio-unix-2.0 json-glib-1.0) -Wall -Wextra -O2
LDFLAGS = $(shell $(PKG_CONFIG) --libs gio-2.0 gio-unix-2.0 json-glib-1.0)

activity-tracker: activity-tracker.c tracker-core.o discord-ipc.o
	$(CC) $(CFLAGS) -o $@ activity-tracker.c tracker-core.o discord-ipc.o $(LDFLAGS)

tracker-core.o: tracker-core.c tracker-core.h
	$(CC) $(CFLAGS) -c -o $@ tracker-core.c

discord-ipc.o: discord-ipc.c discord-ipc.h tracker-core.h
	$(CC) $(CFLAGS) -c -o $@ discord-ipc.c

test-tracker: test-tracker.c tracker-core.o
	$(CC) $(CFLAGS) -o $@ test-tracker.c tracker-core.o $(LDFLAGS)

test-discord-ipc: test-discord-ipc.c discord-ipc.o tracker-core.o
	$(CC) $(CFLAGS) -o $@ test-discord-ipc.c discord-ipc.o tracker-core.o $(LDFLAGS)

test: test-tracker test-discord-ipc
	./test-tracker
	./test-discord-ipc

clean:
	rm -f activity-tracker test-tracker test-discord-ipc tracker-core.o discord-ipc.o

.PHONY: clean test

RESTART_BIN = activity-tracker

restart: activity-tracker
	@if pkill -0 -f '^\./$(RESTART_BIN)' 2>/dev/null; then \
		echo "Stopping existing $(RESTART_BIN)..."; \
		pkill -f '^\./$(RESTART_BIN)'; \
		while pkill -0 -f '^\./$(RESTART_BIN)' 2>/dev/null; do \
			sleep 0.1; \
		done; \
		echo "Process stopped."; \
		sleep 0.5; \
		echo "Locks released."; \
	fi
	@echo "Starting new version..."
	@./$(RESTART_BIN) &
