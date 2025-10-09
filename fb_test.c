/**
 * @file fb_test.c
 * @author Lorang Strand
 * @date 2025-10-09
 * @brief Test program for framebuffer access
 * @version 0.1
 * This file is part of the Stetris project.
 * It is used to test the framebuffer access on a Raspberry Pi with Sense HAT.
 * It opens the framebuffer device and maps it to memory.
 * Set specified pixel (x, y) to a color. 
 * (x, y) should be given as command line arguments. range x, y: [0..7]
 */

#define _GNU_SOURCE
#define DEV_FB "/dev"
#define FB_DEV_NAME "fb"
#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/input.h>
#include <poll.h>
#include <dirent.h>
#include <sys/mman.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <inttypes.h>

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

struct fb_t {
    uint16_t pixel[8][8];
};

struct fb_t *fb = NULL;    // Pointer to framebuffer memory
int fbfd = 0; // framebuffer file descriptor

// Poll structure for event device (joystick)
struct pollfd evpoll = {
    .events = POLLIN,
};

bool running = true;

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

void normalize_string(char *str) {
    // Trim leading whitespace
    while(isspace((unsigned char)*str)) str++;

    // Trim trailing whitespace
    char *end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    // Convert to lowercase
    for (char *p = str; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
}

color_t parse_color(const char *color_str) {

    // Make a copy of the input string to normalize
    char color_str_copy[32];
    strncpy(color_str_copy, color_str, sizeof(color_str_copy) - 1);
    normalize_string(color_str_copy);

    // Match normalized string to color enum
    if (strcmp(color_str_copy, "red") == 0) return red;
    if (strcmp(color_str_copy, "green") == 0) return green;
    if (strcmp(color_str_copy, "blue") == 0) return blue;
    if (strcmp(color_str_copy, "magenta") == 0) return magenta;
    if (strcmp(color_str_copy, "cyan") == 0) return cyan;
    if (strcmp(color_str_copy, "yellow") == 0) return yellow;
    if (strcmp(color_str_copy, "black") == 0) return black;
    if (strcmp(color_str_copy, "white") == 0) return white;
    return black; // Default to black if unknown
}

/**
 * Returns the key code of the event read from the event device.
 * Returns KEY_ENTER on read error.
 * Returns KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER for respective joystick directions.
 */
unsigned int event(int evfd)
{
	struct input_event event[64];
	int i, rd;

	rd = read(evfd, event, sizeof(struct input_event) * 64);
	if (rd < (int) sizeof(struct input_event)) 
    {
		fprintf(stderr, "expected %d bytes, got %d\n", (int) sizeof(struct input_event), rd);
		return KEY_ENTER; // Return ENTER key code on read error
	}
	for (i = 0; i < rd / sizeof(struct input_event); i++) {
		if (event[i].type != EV_KEY)
			continue;   // only consider key events, not other event types
		if (event[i].value != 1)
			continue;   // only consider key press events, not releases or repeats
                
        // Return the key code for the pressed key
		return event[i].code;
	}
}


/**
 * Initializes the Sense HAT by opening the framebuffer and event devices.
 * Maps the framebuffer to memory for pixel manipulation.
 */
void initilize_SenseHat()
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

/**
 * Frees resources allocated for Sense HAT access.
 */
void free_SenseHat()
{
    memset(fb, 0, 128); // Clear framebuffer (turn all pixels off (black))
    if (fb)
        munmap(fb, 128); // Unmap framebuffer memory
    if (fbfd > 0)
        close(fbfd); // Close framebuffer file descriptor
    if (evpoll.fd >= 0)
        close(evpoll.fd); // Close event device file descriptor
}

int main(int argc, char *argv[])
{
    initilize_SenseHat();

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <x> <y> <color>\n", argv[0]);
        fprintf(stderr, "x, y: pixel coordinates (0-7)\n");
        fprintf(stderr, "color: red, green, blue, magenta, cyan, yellow, black, white\n");
        return EXIT_FAILURE;
    }
    int x = atoi(argv[1]);
    int y = atoi(argv[2]);
    if (x < 0 || x > 7 || y < 0 || y > 7) 
    {
        fprintf(stderr, "Error: x and y must be in the range [0..7]\n");
        return EXIT_FAILURE;
    }

    char *color_str = argv[3];
    color_t color = parse_color(color_str);
    if (color == black && strcmp(color_str, "black") != 0) 
    {
        fprintf(stderr, "Error: Unknown color '%s'\n", color_str);
        return EXIT_FAILURE;
    }

    fb->pixel[y][x] = color; // Set specified pixel to the given color
    printf("Set pixel (%d, %d) to color %s (0x%04X)\n", x, y, color_str, color);
    
    while (running)
    {
        while(poll(&evpoll, 1, 100) > 0)
        {
            unsigned int key = event(evpoll.fd);
            switch (key)
            {
                case KEY_ESC:
                    running = false; // Exit on ESC key
                    fprintf(stdout, "ESC pressed, exiting...\n");
                    break;
                case KEY_ENTER:
                    running = false; // Exit on ENTER key
                    break;
                case KEY_UP:
                    fprintf(stdout, "UP\n");
                    break;
                case KEY_DOWN:
                    fprintf(stdout, "DOWN\n");
                    break;
                case KEY_RIGHT:
                    fprintf(stdout, "RIGHT\n");
                    break;
                case KEY_LEFT:
                    fprintf(stdout, "LEFT\n");
                    break;
                default:
                    fprintf(stdout, "Other key: %u\n", key);
            }
            
        }
    }
    free_SenseHat();
    return EXIT_SUCCESS;
}