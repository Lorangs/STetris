/**
 * @file stetris_rpi_and_console.c
 * @author Lorang Strand
 * @date 2025-10-09
 * @brief A Tetris clone for Raspberry Pi with Sense HAT and console output.
 * @version 0.1
 * This file is part of the Stetris project.
 * It implements a Tetris clone that can run on a Raspberry Pi with Sense HAT
 * or in a console. The game logic is the same for both versions, but the input
 * and output functions are different.
 * The Sense HAT version uses the Sense HAT joystick for input and the LED
 * matrix for output. The console version uses the keyboard for input and
 * ANSI escape codes for output.
 */


#define _GNU_SOURCE         
#define DEV_FB "/dev"       // Framebuffer device directory
#define FB_DEV_NAME "fb"    // Framebuffer device name prefix
#define DEV_INPUT_EVENT "/dev/input"    // Input event device directory (for joystick)
#define EVENT_DEV_NAME "event"          // Input event device name prefix (for joystick)
#define BLOCK_COLOR red     // Color for the blocks in the game

#include <stdbool.h>    // for bool type
#include <linux/fb.h>   // for framebuffer structures
#include <linux/input.h>// for input event structures
#include <dirent.h>     // for scandir()
#include <fcntl.h>      // for open()
#include <inttypes.h>   // for inttypes macros
#include <limits.h>     // for PATH_MAX
#include <stdio.h>      // for printf(), snprintf()
#include <stdlib.h>     // for malloc(), free(), exit()
#include <unistd.h>     // for close(), read(), write()
#include <sys/select.h> // for select()
#include <string.h>     // for strncmp, strcmp, strlen
#include <sys/mman.h>   // for mmap (memory mapping)
#include <time.h>       // for nanosleep
#include <poll.h>       // for non-blocking input handling
#include <termios.h>    // for console input handling
#include <signal.h>     // for signal handling

// The game state can be used to detect what happens on the playfield
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

struct fb_t *fb = NULL;    // Pointer to framebuffer memory
int fbfd = 0; // framebuffer file descriptor

// Poll structure for event device (joystick)
struct pollfd evpoll = {
    .events = POLLIN,
};

// Global variable to store original terminal settings
struct termios old_termios, new_termios;


// Function prototypes
void cleanUp();
void interrupt_handler(int signum);
static int is_event_device(const struct dirent *dir);
static int is_framebuffer_device(const struct dirent *dir);
static int open_fbdev(const char *dev_name);
static int open_evdev(const char *dev_name);
void initializeSenseHat();
int readJoystick(struct input_event *ev);
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


// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target)
{
    game.playfield[target.y][target.x].occupied = true;
}

static inline void copyTile(coord const to, coord const from)
{
    memcpy((void *)&game.playfield[to.y][to.x], (void *)&game.playfield[from.y][from.x], sizeof(tile));
}

static inline void copyRow(unsigned int const to, unsigned int const from)
{
    memcpy((void *)&game.playfield[to][0], (void *)&game.playfield[from][0], sizeof(tile) * game.grid.x);
}

static inline void resetTile(coord const target)
{
    memset((void *)&game.playfield[target.y][target.x], 0, sizeof(tile));
}

static inline void resetRow(unsigned int const target)
{
    memset((void *)&game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
}

static inline bool tileOccupied(coord const target)
{
    return game.playfield[target.y][target.x].occupied;
}

static inline bool rowOccupied(unsigned int const target)
{
    for (unsigned int x = 0; x < game.grid.x; x++)
    {
        coord const checkTile = {x, target};
        if (!tileOccupied(checkTile))
        {
            return false;
        }
    }
    return true;
}

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
void interrupt_handler(int signum)
{
    cleanUp();
    fprintf(stderr, "\nInterrupt signal (%d) received. Exiting...\n", signum);
    exit(EXIT_SUCCESS);
}

/**
 * Checks if the given directory entry is an event device.
 */
static int is_event_device(const struct dirent *dir)
{
    return strncmp(EVENT_DEV_NAME, dir->d_name, strlen(EVENT_DEV_NAME)-1) == 0;
}

/**
 * Checks if the given directory entry is a framebuffer device.
 */
static int is_framebuffer_device(const struct dirent *dir)
{
    return strncmp(FB_DEV_NAME, dir->d_name, strlen(FB_DEV_NAME)-1) == 0;
}


/**
 * Opens the framebuffer device with the given name.
 */
static int open_fbdev(const char *dev_name)
{

    struct dirent **namelist; // list of directory entries
    int i, ndev;            // number of devices found
    int fd = -1;            // file descriptor to return
    struct fb_fix_screeninfo fix_info;  // fixed screen info structure

    ndev = scandir(DEV_FB, &namelist, is_framebuffer_device, versionsort);  // scan for framebuffer devices
    if (ndev <= 0)
        return ndev;

    // iterate over all devices found
    for (i = 0; i < ndev; i++)
    {
        char fname[PATH_MAX];     // filename buffer
        snprintf(fname, sizeof(fname), "%s/%s", DEV_FB, namelist[i]->d_name);   // construct full path
        fd = open(fname, O_RDWR);        // open device with read/write access
        // if open failed, try next device
        if (fd < 0)
            continue;
        ioctl(fd, FBIOGET_FSCREENINFO, &fix_info); // load fixed screen info into fix_info structure
        if (strcmp(dev_name, fix_info.id) == 0)  // Check device name to match for desired device (Sense HAT FB)
            break;
        close(fd);  // close device if not the desired one
        fd = -1;    // reset file descriptor
    }
    for (i = 0; i < ndev; i++)
        free(namelist[i]); // free allocated memory for directory entries
    
    return fd;  // return file descriptor of the opened device or -1 if not found
}


/**
 * Opens the event device with the given name.
 */
static int open_evdev(const char *dev_name)
{
    struct dirent **namelist;       // list of directory entries
    int i, ndev;                    // number of devices found
    int fd = -1;                    // file descriptor to return

    // scan for event devices, sorted by version
    ndev = scandir(DEV_INPUT_EVENT, &namelist, is_event_device, versionsort);
    if (ndev <= 0)
        return ndev;    // return errormessage if no devices found

    // iterate over all devices found
    for (i = 0; i < ndev; i++)
    {
        char fname[PATH_MAX];
        char name[256];

        // construct full path to device
        snprintf(fname, sizeof(fname), "%s/%s", DEV_INPUT_EVENT, namelist[i]->d_name);
        
        // open device with read-only access
        fd = open(fname, O_RDONLY);
        if (fd < 0)
            continue;   // if open failed, try next device
        
        // get device name
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strcmp(dev_name, name) == 0)
            break;  // if device name matches, break loop and keep fd
        close(fd);
        fd = -1;
    }
    // free allocated memory for directory entries
    for (i = 0; i < ndev; i++)
        free(namelist[i]);

    return fd;
}


// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true
void initializeSenseHat()
{
        // Open framebuffer device
    fbfd = open_fbdev("RPi-Sense FB"); // Open framebuffer device
    if (fbfd <= 0)
    {
        fprintf(stderr, "ERROR: cannot open framebuffer device. ErrorCode:\t%i\n", fbfd);
        exit(EXIT_FAILURE);
    }
    fb = (struct fb_t *)mmap(0, 128, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0); // Map framebuffer to memory
    if (fb == MAP_FAILED)
    {
        fprintf(stderr, "ERROR: Failed to mmap framebuffer.\n");
        fb = NULL;
        close(fbfd);
        exit(EXIT_FAILURE);
    }
    if (fb)
    {
        memset(fb, 0, 128); // Clear framebuffer (turn all pixels off (black))
    }
    else
    {
        fprintf(stderr, "ERROR: Framebuffer pointer is NULL.\n");
        close(fbfd);
        exit(EXIT_FAILURE);
    }
    fprintf(stdout, "DEBUG: Framebuffer initialized successfully.\n");

    // open event device (joystick)
    evpoll.fd = open_evdev("Raspberry Pi Sense HAT Joystick");
    if (evpoll.fd < 0)
    {
        fprintf(stderr, "ERROR: Event device not found.\n");
        munmap(fb, 128); // Unmap framebuffer memory
        close(fbfd);     // Close framebuffer file descriptor
        exit(EXIT_FAILURE);
    }
    else
    {
        fprintf(stdout, "DEBUG: Event device initialized successfully.\n");
    }
}

// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat()
{
    memset(fb, 0, 128); // Clear framebuffer (turn all pixels off (black))
    if (fb)
        munmap(fb, 128); // Unmap framebuffer memory
    if (fbfd > 0)
        close(fbfd); // Close framebuffer file descriptor
    if (evpoll.fd >= 0)
        close(evpoll.fd); // Close event device file descriptor
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

    freeSenseHat();
    free(game.rawPlayfield);
    free(game.playfield);
}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
int readSenseHatJoystick()
{
    struct input_event ev[64];
    int i, key;

    if (!poll(&evpoll, 1, 0)) // Poll event device with no timeout
        return 0; // No event available

    key = read(evpoll.fd, ev, sizeof(struct input_event) * 64);
    if (key < (int) sizeof(struct input_event)) 
    {
        // No complete event available, not an error
        fprintf(stderr, "expected %d bytes, got %d\n", (int) sizeof(struct input_event), key);
        return 0;
    }
    for (i = 0; i < (int)(key / sizeof(struct input_event)); i++) {
        if (ev[i].type != EV_KEY)
            continue;
        if (ev[i].value != 1)
            continue;
        switch (ev[i].code) {
            case KEY_ENTER:
                return KEY_ENTER;
            case KEY_UP:
                return KEY_UP;
            case KEY_DOWN:
                return KEY_DOWN;
            case KEY_RIGHT:
                return KEY_RIGHT;
            case KEY_LEFT:
                return KEY_LEFT;
        }
    }
    return 0;
}

// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged)
{
    if (playfieldChanged)
    {
        for (unsigned int y = 0; y < game.grid.y; y++)
        {
            for (unsigned int x = 0; x < game.grid.x; x++)
            {
                if (game.playfield[y][x].occupied)
                {
                    fb->pixel[y][x] = BLOCK_COLOR; // Set pixel to BLOCK_COLOR if occupied
                }
                else
                {
                    fb->pixel[y][x] = black; // Set pixel to black if not occupied
                }
            }
        }
    }
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



// Below here comes the game logic. Keep in mind: You are not allowed to change how the game works!
// that means no changes are necessary below this line! And if you choose to change something
// keep it compatible with what was provided to you!

bool addNewTile()
{
    game.activeTile.y = 0;
    game.activeTile.x = (game.grid.x - 1) / 2;
    if (tileOccupied(game.activeTile))
        return false;
    newTile(game.activeTile);
    return true;
}

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

void gameOver()
{
    game.state = GAMEOVER;
    game.nextGameTick = game.initNextGameTick;
}

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

inline unsigned long uSecFromTimespec(struct timespec const ts)
{
    return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

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
    signal(SIGINT, interrupt_handler);   // Ctrl+C
    signal(SIGTERM, interrupt_handler);  // Termination signal

    // Reset playfield to make it empty
    resetPlayfield();
    // Start with gameOver
    gameOver();

    initializeSenseHat();

    // Clear console, render first time
    fprintf(stdout, "\033[H\033[J");
    renderConsole(true);
    renderSenseHatMatrix(true);

    while (true)
    {
        struct timeval sTv, eTv;
        gettimeofday(&sTv, NULL);

        int key = readSenseHatJoystick();
        if (!key)
        {
            key = readKeyboard();
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
