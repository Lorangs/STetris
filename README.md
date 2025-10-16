# STetris - Tetris Game for Raspberry Pi Sense HAT

A colorful Tetris implementation designed for the Raspberry Pi 4 with Sense HAT expansion board, featuring both hardware LED matrix display and console versions.

## Project Structure

### Game Implementations
- **`stetris.c`** - Sense HAT version with LED matrix display and joystick input
- **`stetris_console.c`** - Console-only version for testing and development
- **`stetris_rpi_and_console.c`** - Hybrid version supporting both Sense HAT and keyboard input

### Development Files
- **`stetris_skeleton.c`** - Original skeleton code provided for the assignment
- **`fb_test.c`** - Framebuffer testing utility for debugging LED matrix functionality

### Build System
- **`Makefile`** - Build configuration for all targets

### Example Code
- **`example_senseHat/`** - Reference implementation copied from Raspberry Pi Sense HAT examples found online

## Compilation

### Build All Targets
```bash
make
```

### Indicidual Targets
```bash
# Sense HAT version (requires hardware)
make stetris

# Console version (works on any Linux system)  
make stetris_console

# Hybrid version
make stetris_rpi_and_console

# Testing utility
make fb_test

# Clean build files
make clean
```

## Usage

### Console Version (Development/Testing)
```bash
./stetris_console
# Controls: Arrow keys to move, Enter to exit
```

### Sense HAT Version (Raspberry Pi 4)
```bash
./stetris
# Controls: Sense HAT joystick + keyboard fallback
# Display: 8×8 RGB LED matrix + console output
```

### Framebuffer Test Utility
```bash
./fb_test <x> <y> <color>
# Example: ./fb_test 3 4 red
# Sets pixel at position (3,4) to red color
```


## Features

- **Colorful Blocks**: 6 distinct colors (red, green, blue, magenta, cyan, yellow)
- **Progressive Difficulty**: Game speed increases with level
- **Row Clearing**: Standard Tetris mechanics with animations
- **Score System**: Points and statistics tracking
- **Dual Input**: Joystick and keyboard support
- **Signal Handling**: Clean exit with terminal restoration
- **8×8 Playfield**: Optimized for Sense HAT LED matrix

## Hardware Requirements

### Console Version
- Any Linux system with terminal support

### Sense HAT Version
- Raspberry Pi 4 (or compatible)
- Sense HAT expansion board properly connected
- Linux with framebuffer and input device support

## Game Controls

| Action | Keyboard | Sense HAT Joystick |
|--------|----------|-------------------|
| Move Left | ← Arrow | Left |
| Move Right | → Arrow | Right |
| Drop Down | ↓ Arrow | Down |
| Exit Game | Enter | - |
| New Game | Any key (when game over) | Any direction |
| Force Exit | Ctrl+C | Ctrl+C |


## Color Scheme

The game uses a vibrant 6-color palette displayed as both RGB colors on the LED matrix and ASCII characters in console mode:

| Color | RGB565 | Console Character |
|-------|--------|-------------------|
| Red | 0xF800 | @ |
| Green | 0x07E0 | # |
| Blue | 0x001F | * |
| Magenta | 0xF81F | % |
| Cyan | 0x07FF | & |
| Yellow | 0xFFE0 | $ |

## Technical Implementation

### Device Detection
- Automatic framebuffer device discovery (`/dev/fb*`)
- Dynamic input device enumeration (`/dev/input/event*`)
- Device identification by name matching

### System Requirements
- **Compiler**: GCC with C99 support
- **Libraries**: Standard C library, Linux headers
- **Permissions**: Read/write access to `/dev/fb*` and `/dev/input/*`
- **GNU Extensions**: Requires `_GNU_SOURCE` for `scandir()` and `versionsort()`

### Memory Management
- Direct framebuffer memory mapping via `mmap()`
- Automatic cleanup on program termination
- Signal-safe terminal restoration


## Troubleshooting

### Common Issues

**"Permission denied" errors:**
```bash
# Add user to input group
sudo usermod -a -G input $USER
# Restart session or use sudo
```

**"Device not found" errors:**
```bash
# Check if Sense HAT is properly connected
ls /dev/fb* /dev/input/event*
# Verify device names
cat /proc/bus/input/devices | grep -A5 "Sense HAT"
```

**Terminal becomes unresponsive:**
```bash
# Reset terminal settings
reset
# Or use Ctrl+C to force exit with cleanup
```


## Development Notes

- Game logic uses bit flags for state management (`ACTIVE`, `ROW_CLEAR`, `TILE_ADDED`)
- Non-blocking input via `poll()` for smooth 60+ FPS gameplay
- Terminal raw mode for direct key capture without buffering
- Cross-platform compatibility between development (console) and target (hardware) versions

---

*Note: The `example_senseHat/` directory contains reference code adapted from Raspberry Pi Sense HAT examples available online.*