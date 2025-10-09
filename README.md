# STetris - Simplified Tetris Game

This project contains two versions of a simplified Tetris game:

## Versions

### 1. Sense HAT Version (`stetris.c`)
- **Target**: Raspberry Pi 4 with Sense HAT expansion board
- **Input**: Sense HAT joystick (with keyboard fallback)
- **Output**: 8×8 LED matrix + console display
- **Colors**: Full RGB colors on LED matrix, ASCII characters in console

### 2. Console Version (`stetris_console.c`)  
- **Target**: Any Linux system
- **Input**: Keyboard arrow keys
- **Output**: Console display only
- **Colors**: ASCII characters representing different colors

## Building

```bash
# Build both versions
make all

# Build only Sense HAT version
make stetris

# Build only console version  
make stetris_console

# Clean build files
make clean
```

## Running

### Console Version (for testing)
```bash
./stetris_console
```

### Sense HAT Version (on Raspberry Pi)
```bash
./stetris
```

## Controls

### Keyboard (both versions)
- **Arrow Keys**: Move tiles left/right, drop quickly (down)
- **Enter**: Exit game
- **Any key**: Start new game when game over

### Sense HAT Joystick (stetris only)
- **Joystick directions**: Move tiles left/right, drop quickly (down)  
- **Joystick press**: Exit game
- **Any input**: Start new game when game over

## Game Features

- **Colorful blocks**: Each tile gets a random color
- **Progressive difficulty**: Game speeds up as you clear rows
- **Score system**: Points awarded for clearing rows
- **8×8 playfield**: Optimized for Sense HAT LED matrix

## Color Legend (Console)
- `@` = Red blocks
- `#` = Green blocks  
- `*` = Blue blocks
- `%` = Magenta blocks
- `&` = Cyan blocks
- `$` = Yellow blocks

## Technical Details

Both versions share the same game logic but differ in I/O handling:

- **stetris.c**: Uses Linux framebuffer (`/dev/fb*`) and input event system (`/dev/input/event*`)
- **stetris_console.c**: Uses standard I/O for terminal-based gameplay

The Sense HAT version gracefully falls back to keyboard input if the hardware is not available, making it testable on regular Linux systems.