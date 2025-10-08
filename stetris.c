#define _GNU_SOURCE
#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"
#define DEV_FB "/dev"
#define FB_DEV_NAME "fb"


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>


#include <linux/fb.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <inttypes.h>

// The game state can be used to detect what happens on the playfield
#define GAMEOVER 0
#define ACTIVE (1 << 0)
#define ROW_CLEAR (1 << 1)
#define TILE_ADDED (1 << 2)

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

struct fb_t {
    uint16_t pixel[8][8];
};


// Initial game configuration
gameConfig game = {
    .grid = {8, 8},
    .uSecTickTime = 10000,
    .rowsPerLevel = 2,
    .initNextGameTick = 50,
};


struct fb_t *fb = NULL;    // Pointer to framebuffer memory
int fbfd = 0; // framebuffer file descriptor

// Poll structure for event device (joystick)
struct pollfd evpoll = {
    .events = POLLIN,
};

/**
 * Checks if the given directory entry is a framebuffer device.
 */
static int is_framebuffer_device(const struct dirent *dir)
{
    return strncmp(FB_DEV_NAME, dir->d_name, strlen(FB_DEV_NAME)-1) == 0;
}

/**
 * Checks if the given directory entry is an event device.
 */
static int is_event_device(const struct dirent *dir)
{
    return strncmp(EVENT_DEV_NAME, dir->d_name, strlen(EVENT_DEV_NAME)-1) == 0;
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
        char fname[64];     // filename buffer
        char name[256];     // device name buffer
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
        char fname[64];
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
bool initializeSenseHat()
{
    bool ret = true;
    fbfd = open_fbdev("RPi-Sense FB");  // open framebuffer device
    if (fbfd <= 0) {
        ret = false;
        fprintf(stderr, "ERROR: cannot open framebuffer device. ErrorCode:\t%i\n", fbfd);
    }

    fb = mmap(0, 128, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0); // memory map the framebuffer
    if (!fb) {
        ret = false;
        fprintf(stderr, "ERROR: Failed to mmap framebuffer. ErrorCode:\t%i\n", fb);
    }
    memset(fb, 0, 128); // set all pixels to 0 (white)

    evpoll.fd = open_evdev("Raspberry Pi Sense HAT Joystick"); // open event device
    if (evpoll.fd < 0) {
        ret = false;
        fprintf(stderr, "ERROR: Event device not found.\n");
    }
    return ret;
}

// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat()
{
    if (fb)
        munmap(fb, 128); // unmap framebuffer memory
    if (fbfd > 0)
        close(fbfd); // close framebuffer file descriptor
    if (evpoll.fd >= 0)
        close(evpoll.fd); // close event device file descriptor
}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
int readSenseHatJoystick()
{
    struct input_event ev[64];
    int i, rd;

    rd = read(evpoll.fd, ev, sizeof(struct input_event) * 64);
    if (rd < (int) sizeof(struct input_event)) {
        fprintf(stderr, "expected %d bytes, got %d\n", (int) sizeof(struct input_event), rd);
        return -1;
    }
    for (i = 0; i < rd / sizeof(struct input_event); i++) {
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
            default: 
                break;
        }
    }               
    return 0;
}

// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged)
{
    if (!playfieldChanged)
        return;

    // Clear framebuffer
    memset(fb, 0, 128);

    // Render playfield to framebuffer
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        for (unsigned int x = 0; x < game.grid.x; x++)
        {
            fb->pixel[x][y] = game.playfield[y][x].occupied ? 0xFFFF : 0; // white for occupied, black for empty
        }
    }
}

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
        return KEY_ENTER;
    case 65:
        return KEY_UP;
    case 66:
        return KEY_DOWN;
    case 67:
        return KEY_RIGHT;
    case 68:
        return KEY_LEFT;
    }
    return 0;
}

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

inline unsigned long uSecFromTimespec(struct timespec const ts)
{
    return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    // This sets the stdin in a special state where each
    // keyboard press is directly flushed to the stdin and additionally
    // not outputted to the stdout
    {
        struct termios ttystate;
        tcgetattr(STDIN_FILENO, &ttystate);
        ttystate.c_lflag &= ~(ICANON | ECHO);
        ttystate.c_cc[VMIN] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
    }

    // Allocate the playing field structure
    game.rawPlayfield = (tile *)malloc(game.grid.x * game.grid.y * sizeof(tile));
    game.playfield = (tile **)malloc(game.grid.y * sizeof(tile *));
    if (!game.playfield || !game.rawPlayfield)
    {
        fprintf(stderr, "ERROR: could not allocate playfield\n");
        return 1;
    }
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
    }

    // Reset playfield to make it empty
    resetPlayfield();
    // Start with gameOver
    gameOver();

    if (!initializeSenseHat())
    {
        fprintf(stderr, "ERROR: could not initilize sense hat\n");
        return 1;
    };

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
            // NOTE: Uncomment the next line if you want to test your implementation with
            // reading the inputs from stdin. However, we expect you to read the inputs directly
            // from the input device and not from stdin (you should implement the readSenseHatJoystick
            // method).
            // key = readKeyboard();
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

    freeSenseHat();
    free(game.playfield);
    free(game.rawPlayfield);

    return 0;
}
