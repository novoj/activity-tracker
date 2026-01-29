CC = gcc
CFLAGS = $(shell pkg-config --cflags gio-2.0 gio-unix-2.0 json-glib-1.0) -Wall -Wextra -O2
LDFLAGS = $(shell pkg-config --libs gio-2.0 gio-unix-2.0 json-glib-1.0)

activity-tracker: activity-tracker.c tracker-core.o
	$(CC) $(CFLAGS) -o $@ activity-tracker.c tracker-core.o $(LDFLAGS)

tracker-core.o: tracker-core.c tracker-core.h
	$(CC) $(CFLAGS) -c -o $@ tracker-core.c

test-tracker: test-tracker.c tracker-core.o
	$(CC) $(CFLAGS) -o $@ test-tracker.c tracker-core.o $(LDFLAGS)

test: test-tracker
	./test-tracker

clean:
	rm -f activity-tracker test-tracker tracker-core.o

.PHONY: clean test

RESTART_BIN = activity-tracker

restart: activity-tracker
	@if pkill -0 -f ./$(RESTART_BIN) 2>/dev/null; then \
		echo "Stopping existing $(RESTART_BIN)..."; \
		pkill -f ./$(RESTART_BIN); \
		while pkill -0 -f ./$(RESTART_BIN) 2>/dev/null; do \
			sleep 0.1; \
		done; \
		echo "Process stopped and locks released."; \
	fi
	@echo "Starting new version..."
	@./$(RESTART_BIN) &