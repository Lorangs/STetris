/**
 * @file stetris_rpi_and_console.c
 * @author Lorang Strand
 * @date 2025-10-09
 * @brief A Tetris clone for Raspberry Pi with Sense HAT and console output.
 * @version 1.0
 * This file is part of the Stetris project.
 * It implements a Tetris clone that can run on a Raspberry Pi with Sense HAT
 * or in a console. The game logic is the same for both versions, but the input
 * and output functions are different.
 * The Sense HAT version uses the Sense HAT joystick for input and the LED
 * matrix for output. The console version uses the keyboard for input and
 * ANSI escape codes for output.
 */


#define _GNU_SOURCE                     // Enables scandir() and versionsort() 
#define DEV_FB          "/dev"          // Framebuffer device directory
#define FB_DEV_NAME     "fb"            // Framebuffer device name prefix
#define DEV_INPUT_EVENT "/dev/input"    // Input event device directory (for joystick)
#define EVENT_DEV_NAME  "event"         // Input event device name prefix (for joystick)
#define BLOCK_COLOR     red             // Color for the blocks in the game

#include <stdbool.h>                    // for bool type
#include <linux/fb.h>                   // for framebuffer structures
#include <linux/input.h>                // for input event structures
#include <dirent.h>                     // for scandir()
#include <fcntl.h>                      // for open()
#include <inttypes.h>                   // for inttypes macros
#include <limits.h>                     // for PATH_MAX
#include <stdio.h>                      // for printf(), snprintf()
#include <stdlib.h>                     // for malloc(), free(), exit()
#include <unistd.h>                     // for close(), read(), write()
#include <sys/select.h>                 // for select()
#include <string.h>                     // for strncmp, strcmp, strlen
#include <sys/mman.h>                   // for mmap (memory mapping)
#include <time.h>                       // for nanosleep
#include <poll.h>                       // for non-blocking input handling
#include <termios.h>                    // for console input handling
#include <signal.h>                     // for signal handling

/**
 * Game state bit field definitions.
 * These can be combined using bitwise OR to represent multiple states.'
 * 
 * Structure of game.state:
 * Bit Position:  7 6 5 4 3 2 1 0
 *                │ │ │ │ │ │ │ │
 *                │ │ │ │ │ │ │ └─ ACTIVE       (bit 0)
 *                │ │ │ │ │ │ └─── ROW_CLEAR    (bit 1)
 *                │ │ │ │ │ └───── TILE_ADDED   (bit 2)
 *                │ │ │ │ └─────── (unused)
 *                │ │ │ └───────── (unused)
 *                │ │ └─────────── (unused)
 *                │ └───────────── (unused)
 *                └─────────────── (unused)
 * 
 */
#define GAMEOVER 0
#define ACTIVE (1 << 0)
#define ROW_CLEAR (1 << 1)
#define TILE_ADDED (1 << 2)

typedef enum color {
    red = 0xF800,
    green = 0x07E0,
    blue = 0x001F,
    magenta = 0xF81F,
    cyan = 0x07FF,
    yellow = 0xFFE0,
    black = 0x0000,
    white = 0xFFFF,
} color_t;


// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct
{
    bool occupied;
} tile;

typedef struct
{
    unsigned int x;
    unsigned int y;
} coord;

typedef struct
{
    coord const grid;                     // playfield bounds
    unsigned long const uSecTickTime;     // tick rate
    unsigned long const rowsPerLevel;     // speed up after clearing rows
    unsigned long const initNextGameTick; // initial value of nextGameTick

    unsigned int tiles; // number of tiles played
    unsigned int rows;  // number of rows cleared
    unsigned int score; // game score
    unsigned int level; // game level

    tile *rawPlayfield; // pointer to raw memory of the playfield
    tile **playfield;   // This is the play field array
    unsigned int state;
    coord activeTile; // current tile

    unsigned long tick;         // incremeted at tickrate, wraps at nextGameTick
                                // when reached 0, next game state calculated
    unsigned long nextGameTick; // sets when tick is wrapping back to zero
                                // lowers with increasing level, never reaches 0
} gameConfig;

gameConfig game = {
    .grid = {8, 8},
    .uSecTickTime = 10000,
    .rowsPerLevel = 2,
    .initNextGameTick = 50,
};
struct fb_t {
    uint16_t pixel[8][8];
};

struct fb_t *fb = NULL;     // Pointer to framebuffer memory
int fbfd = 0;               // framebuffer file descriptor

// Poll structure for event device (joystick)
struct pollfd evpoll = {
    .events = POLLIN,
};

// Global variable to store original terminal settings
struct termios old_termios, new_termios;


// Function prototypes
void cleanUp();
void interuptHandler(int signum);
int readKeyboard();
void resetGame();
void drawPlayfield();
void drawPixel(unsigned int x, unsigned int y, color_t color);
void drawBlock(unsigned int x, unsigned int y, color_t color);
void drawBlockAtActiveTile(color_t color);
void clearBlockAtActiveTile();
bool moveTile(int dx, int dy);
void rotateTile();
bool tileFits(int x, int y);
void placeTile();
bool clearFullRows();
void gameTick();
void gameLoop();
void renderConsole(bool const playfieldChanged);
bool sTetris(int const key);


/**
 * Creates a new tile at the specified coordinates by marking the tile as occupied.
 */
static inline void newTile(coord const target)
{
    game.playfield[target.y][target.x].occupied = true;
}

/**
 * Copies the tile from one coordinate to another.
 */
static inline void copyTile(coord const to, coord const from)
{
    memcpy((void *)&game.playfield[to.y][to.x], (void *)&game.playfield[from.y][from.x], sizeof(tile));
}

/**
 * Copies an entire row from one index to another.
 */
static inline void copyRow(unsigned int const to, unsigned int const from)
{
    memcpy((void *)&game.playfield[to][0], (void *)&game.playfield[from][0], sizeof(tile) * game.grid.x);
}

/**
 * Resets the tile at the specified coordinates by marking it as unoccupied.
 */
static inline void resetTile(coord const target)
{
    memset((void *)&game.playfield[target.y][target.x], 0, sizeof(tile));
}

/**
 * Resets an entire row at the specified index by marking all tiles as unoccupied.
 */
static inline void resetRow(unsigned int const target)
{
    memset((void *)&game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
}

/**
 * Checks if the tile at the specified coordinates is occupied.
 * Returns true if occupied, false otherwise.
 */
static inline bool tileOccupied(coord const target)
{
    return game.playfield[target.y][target.x].occupied;
}

/**
 * Checks if the entire row at the specified index is occupied.
 * Returns true if all tiles in the row are occupied, false otherwise.
 */
static inline bool rowOccupied(unsigned int const target)
{
    bool ret = true;
    for (unsigned int x = 0; x < game.grid.x; x++)
    {
        coord const checkTile = {x, target};
        if (!tileOccupied(checkTile))
        {
            ret = false;
            break;
        }
    }
    return ret;
}

/**
 * Resets the entire playfield by marking all tiles as unoccupied.
 */
static inline void resetPlayfield()
{
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        resetRow(y);
    }
}

/**
 * Signal handler for interrupt signal (Ctrl+C).
 */
void interuptHandler(int signum)
{
    cleanUp();
    fprintf(stderr, "\nInterrupt signal (%d) received. Exiting...\n", signum);
    exit(EXIT_SUCCESS);
}



/**
 * Cleans up allocated resources for the game.
 * This function is called on program exit to ensure
 * that all resources are properly released.
 */
void cleanUp()
{
    // Clear console on exit
    fprintf(stdout, "\033[H\033[J");

    // restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
    free(game.rawPlayfield);
    free(game.playfield);
}



/**
 * Reads keyboard input and maps specific keys to game actions.
 * Supports arrow keys for movement and Enter key for game start.
 * Returns 0 if no relevant key is pressed.
 */
int readKeyboard()
{
    struct pollfd pollStdin = {
        .fd = STDIN_FILENO,
        .events = POLLIN};
    int lkey = 0;

    if (poll(&pollStdin, 1, 0))
    {
        lkey = fgetc(stdin);
        if (lkey != 27)
            goto exit;
        lkey = fgetc(stdin);
        if (lkey != 91)
            goto exit;
        lkey = fgetc(stdin);
    }
exit:
    switch (lkey)
    {
        case 10:
            lkey = KEY_ENTER;
            break;
        case 65:
            lkey = KEY_UP;
            break;
        case 66:
            lkey = KEY_DOWN;
            break;
        case 67:
            lkey = KEY_RIGHT;
            break;
        case 68:
            lkey = KEY_LEFT;
            break;
        default:
            lkey = 0;
    }
    return lkey;
}

/**
 * Renders the game state to the console if the playfield has changed.
 * Displays the playfield grid along with game statistics such as tiles,
 * rows, score, level, and game over message if applicable.
 */
void renderConsole(bool const playfieldChanged)
{
    if (!playfieldChanged)
        return;

    // Goto beginning of console
    fprintf(stdout, "\033[%d;%dH", 0, 0);
    for (unsigned int x = 0; x < game.grid.x + 2; x++)
    {
        fprintf(stdout, "-");
    }
    fprintf(stdout, "\n");
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        fprintf(stdout, "|");
        for (unsigned int x = 0; x < game.grid.x; x++)
        {
            coord const checkTile = {x, y};
            fprintf(stdout, "%c", (tileOccupied(checkTile)) ? '#' : ' ');
        }
        switch (y)
        {
        case 0:
            fprintf(stdout, "| Tiles: %10u\n", game.tiles);
            break;
        case 1:
            fprintf(stdout, "| Rows:  %10u\n", game.rows);
            break;
        case 2:
            fprintf(stdout, "| Score: %10u\n", game.score);
            break;
        case 4:
            fprintf(stdout, "| Level: %10u\n", game.level);
            break;
        case 7:
            fprintf(stdout, "| %17s\n", (game.state == GAMEOVER) ? "Game Over" : "");
            break;
        default:
            fprintf(stdout, "|\n");
        }
    }
    for (unsigned int x = 0; x < game.grid.x + 2; x++)
    {
        fprintf(stdout, "-");
    }
    fflush(stdout);
}


/**
 * Adds a new tile to the playfield at the top center position.
 * If the position is already occupied, the function returns false,
 * indicating that a new tile cannot be added (game over condition).
 * Otherwise, it places the new tile and returns true.
 */
bool addNewTile()
{
    game.activeTile.y = 0;
    game.activeTile.x = (game.grid.x - 1) / 2;
    if (tileOccupied(game.activeTile))
        return false;
    newTile(game.activeTile);
    return true;
}

/**
 * Moves the active tile one position to the right if possible.
 * Returns true if the move was successful, false otherwise.
 */
bool moveRight()
{
    coord const newTile = {game.activeTile.x + 1, game.activeTile.y};
    if (game.activeTile.x < (game.grid.x - 1) && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

/**
 * Moves the active tile one position to the left if possible.
 * Returns true if the move was successful, false otherwise.
 */
bool moveLeft()
{
    coord const newTile = {game.activeTile.x - 1, game.activeTile.y};
    if (game.activeTile.x > 0 && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

/**
 * Moves the active tile one position down if possible.
 * Returns true if the move was successful, false otherwise.
 */
bool moveDown()
{
    coord const newTile = {game.activeTile.x, game.activeTile.y + 1};
    if (game.activeTile.y < (game.grid.y - 1) && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

/**
 * Clears the bottom row if it is fully occupied.
 * If row is occupied, all rows above it are shifted down by one,
 * and the top row is reset to empty.
 * Returns true if a row was cleared, false otherwise.
 */
bool clearRow()
{
    if (rowOccupied(game.grid.y - 1))
    {
        for (unsigned int y = game.grid.y - 1; y > 0; y--)
        {
            copyRow(y, y - 1);
        }
        resetRow(0);
        return true;
    }
    return false;
}

/**
 * Advances the game to the next level by incrementing the level counter
 * and adjusting the nextGameTick value to increase game speed.
 */
void advanceLevel()
{
    game.level++;
    switch (game.nextGameTick)
    {
    case 1:
        break;
    case 2 ... 10:
        game.nextGameTick--;
        break;
    case 11 ... 20:
        game.nextGameTick -= 2;
        break;
    default:
        game.nextGameTick -= 10;
    }
}

/**
 * Starts a new game by resetting the game state and playfield.
 * Initializes game parameters such as tiles, rows, score, tick, and level.
 */
void newGame()
{
    game.state = ACTIVE;
    game.tiles = 0;
    game.rows = 0;
    game.score = 0;
    game.tick = 0;
    game.level = 0;
    resetPlayfield();
}

/**
 * Sets the game state to GAMEOVER and resets the nextGameTick to its initial value.
 */
void gameOver()
{
    game.state = GAMEOVER;
    game.nextGameTick = game.initNextGameTick;
}

/**
 * Main game logic function that processes user input and updates the game state.
 * It handles tile movement, row clearing, tile addition, and game over conditions.
 * Returns true if the playfield has changed, false otherwise.
 */
bool sTetris(int const key)
{
    bool playfieldChanged = false;

    if (game.state & ACTIVE)
    {
        // Move the current tile
        if (key)
        {
            playfieldChanged = true;
            switch (key)
            {
            case KEY_LEFT:
                moveLeft();
                break;
            case KEY_RIGHT:
                moveRight();
                break;
            case KEY_DOWN:
                while (moveDown())
                {
                };
                game.tick = 0;
                break;
            default:
                playfieldChanged = false;
            }
        }

        // If we have reached a tick to update the game
        if (game.tick == 0)
        {
            // We communicate the row clear and tile add over the game state
            // clear these bits if they were set before
            game.state &= ~(ROW_CLEAR | TILE_ADDED);

            playfieldChanged = true;
            // Clear row if possible
            if (clearRow())
            {
                game.state |= ROW_CLEAR;
                game.rows++;
                game.score += game.level + 1;
                if ((game.rows % game.rowsPerLevel) == 0)
                {
                    advanceLevel();
                }
            }

            // if there is no current tile or we cannot move it down,
            // add a new one. If not possible, game over.
            if (!tileOccupied(game.activeTile) || !moveDown())
            {
                if (addNewTile())
                {
                    game.state |= TILE_ADDED;
                    game.tiles++;
                }
                else
                {
                    gameOver();
                }
            }
        }
    }

    // Press any key to start a new game
    if ((game.state == GAMEOVER) && key)
    {
        playfieldChanged = true;
        newGame();
        addNewTile();
        game.state |= TILE_ADDED;
        game.tiles++;
    }

    return playfieldChanged;
}

/**
 * Converts a timespec structure to microseconds.
 */
inline unsigned long uSecFromTimespec(struct timespec const ts)
{
    return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}


int main(int argc, char **argv)
{
    (void)argc;     // Unused parameter
    (void)argv;     // Unused parameter

    // Allocate the playing field structure
    game.rawPlayfield = (tile *)malloc(game.grid.x * game.grid.y * sizeof(tile));
    game.playfield = (tile **)malloc(game.grid.y * sizeof(tile *));
    if (!game.playfield || !game.rawPlayfield)
    {
        fprintf(stderr, "ERROR: could not allocate playfield\n");
        return EXIT_FAILURE;
    }
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
    }


    tcgetattr(STDIN_FILENO, &old_termios);  // save current terminal settings
    new_termios = old_termios;          // copy to new settings
    new_termios.c_lflag &= ~(ICANON | ECHO);    // disable canonical mode (buffered i/o) and local echo
    new_termios.c_cc[VMIN] = 1;  // minimum number of characters to read
    new_termios.c_cc[VTIME] = 0; // timeout  
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios); // apply new terminal settings

    // Set up signal handlers for clean exit
    signal(SIGINT, interuptHandler);   // Ctrl+C
    signal(SIGTERM, interuptHandler);  // Termination signal

    // Reset playfield to make it empty
    resetPlayfield();
    // Start with gameOver
    gameOver();

    // Clear console, render first time
    fprintf(stdout, "\033[H\033[J");
    renderConsole(true);


    while (true)
    {
        struct timeval sTv, eTv;
        gettimeofday(&sTv, NULL);

        int key = readKeyboard();
        {
        }
        if (key == KEY_ENTER)
            break;

        bool playfieldChanged = sTetris(key);
        renderConsole(playfieldChanged);
        renderSenseHatMatrix(playfieldChanged);

        // Wait for next tick
        gettimeofday(&eTv, NULL);
        unsigned long const uSecProcessTime = ((eTv.tv_sec * 1000000) + eTv.tv_usec) - ((sTv.tv_sec * 1000000 + sTv.tv_usec));
        if (uSecProcessTime < game.uSecTickTime)
        {
            usleep(game.uSecTickTime - uSecProcessTime);
        }
        game.tick = (game.tick + 1) % game.nextGameTick;
    }
    cleanUp();  
    return EXIT_SUCCESS;
}
