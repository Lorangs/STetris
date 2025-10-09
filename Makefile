CC = gcc
CFLAGS = -Wall -Wextra -std=c99
LDFLAGS = 

# Targets
SENSEHAT_TARGET = stetris
CONSOLE_TARGET = stetris_console

# Source files
SENSEHAT_SRC = stetris.c
CONSOLE_SRC = stetris_console.c

# Build both versions
all: $(SENSEHAT_TARGET) $(CONSOLE_TARGET)

# Sense HAT version (for Raspberry Pi with Sense HAT)
$(SENSEHAT_TARGET): $(SENSEHAT_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Console version (for testing on any system)
$(CONSOLE_TARGET): $(CONSOLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Clean built files
clean:
	rm -f $(SENSEHAT_TARGET) $(CONSOLE_TARGET)

# Install (copy to appropriate location, if needed)
install: all
	@echo "Built $(SENSEHAT_TARGET) for Raspberry Pi with Sense HAT"
	@echo "Built $(CONSOLE_TARGET) for console testing"

# Test the console version
test: $(CONSOLE_TARGET)
	./$(CONSOLE_TARGET)

.PHONY: all clean install test