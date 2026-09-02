/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 duoctl contributors
 *
 * duoctl: Linux command-line utility for WD My Book Duo enclosures.
 *
 * This initial version is a read-only RAID status terminal interface.
 *
 * Safety boundary:
 *   - opens only a SCSI generic character device with O_RDONLY
 *   - issues only WD GetRAIDStatus_Interlaken (A3/1F)
 *   - uses SG_DXFER_FROM_DEV with a fixed 88-byte response
 *   - contains no data-out, rebuild, configuration, erase, or write command
 */

#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_SIZE 88
#define REFRESH_SECONDS 5

#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define RED     "\033[31m"
#define CYAN    "\033[36m"
#define WHITE   "\033[97m"

static struct termios saved_termios;
static int terminal_changed;
static volatile sig_atomic_t stopping;

static void restore_terminal(void)
{
    if (terminal_changed)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    printf("\033[?25h" RESET "\n");
    fflush(stdout);
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int enable_terminal_mode(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_termios) != 0)
        return 0;

    raw = saved_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return 0;

    terminal_changed = 1;
    printf("\033[?25l");
    return 1;
}

static void set_error(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
}

static int read_raid_status(const char *device, unsigned char data[RESPONSE_SIZE],
                            char *error, size_t error_size)
{
    unsigned char sense[64] = {0};
    unsigned char cdb[12] = {
        0xa3, 0x1f, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x58, 0x00, 0x00
    };
    struct stat device_stat;
    sg_io_hdr_t io;
    int descriptor;

    memset(data, 0, RESPONSE_SIZE);
    descriptor = open(device, O_RDONLY);
    if (descriptor < 0) {
        set_error(error, error_size, "Cannot open %s read-only: %s",
                  device, strerror(errno));
        return -1;
    }

    if (fstat(descriptor, &device_stat) != 0 || !S_ISCHR(device_stat.st_mode)) {
        set_error(error, error_size, "Refusing: %s is not a character device", device);
        close(descriptor);
        return -1;
    }

    memset(&io, 0, sizeof(io));
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof(cdb);
    io.mx_sb_len = sizeof(sense);
    io.dxfer_len = RESPONSE_SIZE;
    io.dxferp = data;
    io.cmdp = cdb;
    io.sbp = sense;
    io.timeout = 5000;

    if (ioctl(descriptor, SG_IO, &io) < 0) {
        set_error(error, error_size, "Read-only SG_IO failed: %s", strerror(errno));
        close(descriptor);
        return -1;
    }
    close(descriptor);

    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
        set_error(error, error_size,
                  "Device rejected query: SCSI=0x%02x host=0x%04x driver=0x%04x",
                  io.status, io.host_status, io.driver_status);
        return -1;
    }
    if ((RESPONSE_SIZE - io.resid) < RESPONSE_SIZE) {
        set_error(error, error_size, "Short response: %d of %d bytes",
                  RESPONSE_SIZE - io.resid, RESPONSE_SIZE);
        return -1;
    }
    if (data[0] != 0x31 || data[5] != 2 || data[6] != 1) {
        set_error(error, error_size,
                  "Unexpected layout: signature=0x%02x bays=%u maxvol=%u",
                  data[0], data[5], data[6]);
        return -1;
    }
    return 0;
}

static const char *metadata_name(unsigned value)
{
    switch (value) {
    case 0x00: return "Good";
    case 0x01: return "Conflicting arrays";
    case 0x02: return "Some metadata invalid";
    case 0x03: return "No valid metadata";
    default: return "Unknown";
    }
}

static const char *mode_name(unsigned value)
{
    switch (value) {
    case 0x00: return "RAID 0 (striped)";
    case 0x01: return "RAID 1 (mirrored)";
    case 0x0f: return "JBOD";
    case 0x10: return "Not configured";
    case 0xff: return "Mixed";
    default: return "Unknown";
    }
}

static const char *array_name(unsigned value)
{
    switch (value) {
    case 0x00: return "HEALTHY";
    case 0x01: return "DEGRADED";
    case 0x02: return "REBUILDING";
    case 0x03: return "REBUILD FAILED";
    case 0x04: return "DATA LOSS DETECTED";
    case 0x10: return "NOT CONFIGURED";
    case 0x11: return "CANNOT ACCESS DATA";
    case 0xff: return "MIXED";
    default: return "UNKNOWN";
    }
}

static const char *slot_name(unsigned value)
{
    switch (value) {
    case 0x00: return "Online";
    case 0x01: return "Obsolete / stale";
    case 0x0f: return "Rebuilding";
    case 0x10: return "Empty";
    case 0x11: return "Missing";
    case 0x12: return "Blank / new";
    case 0x13: return "Failed";
    case 0x14: return "Spare";
    case 0x15: return "Not participating";
    case 0x16: return "ID mismatch";
    case 0x17: return "Hard rejected";
    case 0x18: return "Soft rejected";
    default: return "Unknown";
    }
}

static const char *metadata_color(unsigned value)
{
    return value == 0 ? GREEN : (value == 1 || value == 2 ? YELLOW : RED);
}

static const char *array_color(unsigned value)
{
    if (value == 0)
        return GREEN;
    if (value == 1 || value == 2)
        return YELLOW;
    return RED;
}

static const char *slot_color(unsigned value)
{
    if (value == 0)
        return GREEN;
    if (value == 1 || value == 15 || value == 20)
        return YELLOW;
    return RED;
}

static void border(void)
{
    puts(CYAN "+----------------------------------------------------------------------+" RESET);
}

static void row(const char *label, const char *color, const char *value,
                unsigned code)
{
    printf(CYAN "|" RESET "  %-18s " BOLD "%s%-34s" RESET DIM " [0x%02x] " RESET
           CYAN "|" RESET "\n", label, color, value, code);
}

static void progress_bar(unsigned percent)
{
    unsigned filled;
    unsigned position;

    if (percent > 100)
        percent = 100;
    filled = percent * 40 / 100;
    printf(CYAN "|" RESET "  %-18s " YELLOW "[", "Rebuild progress");
    for (position = 0; position < 40; ++position)
        putchar(position < filled ? '#' : '-');
    printf("] %3u%%" RESET "  " CYAN "|" RESET "\n", percent);
}

static void draw_screen(const char *device, const unsigned char data[RESPONSE_SIZE],
                        const char *error)
{
    char timestamp[64];
    time_t now = time(NULL);
    struct tm local_time;
    unsigned volume_sets;
    unsigned index;

    localtime_r(&now, &local_time);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);

    printf("\033[2J\033[H");
    border();
    printf(CYAN "|" RESET BOLD WHITE "                         DUOCTL                                  "
           RESET CYAN "|" RESET "\n");
    border();
    printf(CYAN "|" RESET "  Device: %-30s Updated: %-19s " CYAN "|" RESET "\n",
           device, timestamp);
    printf(CYAN "|" RESET GREEN BOLD
           "  READ ONLY: O_RDONLY + SG_DXFER_FROM_DEV + fixed A3/1F query       "
           RESET CYAN "|" RESET "\n");
    border();

    if (error != NULL) {
        printf(CYAN "|" RESET RED BOLD "  ERROR: %-61.61s" RESET CYAN "|" RESET "\n",
               error);
        border();
        printf(CYAN "|" RESET DIM "  [R] retry                                      [Q] quit  "
               RESET CYAN "|" RESET "\n");
        border();
        fflush(stdout);
        return;
    }

    row("Metadata", metadata_color(data[3]), metadata_name(data[3]), data[3]);
    printf(CYAN "|" RESET "  %-18s " BOLD "%-34u" RESET DIM "        " RESET
           CYAN "|" RESET "\n", "Drive bays", data[5]);
    printf(CYAN "|" RESET "  %-18s " BOLD "%-34u" RESET DIM "        " RESET
           CYAN "|" RESET "\n", "Volume sets", data[7]);
    border();

    volume_sets = data[7] > 2 ? 2 : data[7];
    for (index = 0; index < volume_sets; ++index) {
        unsigned base = 8 + index * 40;
        unsigned array_status = data[base];

        printf(CYAN "|" RESET BOLD "  VOLUME SET %-58u" RESET CYAN "|" RESET "\n",
               index + 1);
        row("Mode", WHITE, mode_name(data[base + 1]), data[base + 1]);
        row("RAID status", array_color(array_status), array_name(array_status),
            array_status);
        if (array_status == 0x02)
            progress_bar(data[base + 3]);
        row("Drive 1 (left)", slot_color(data[base + 36]),
            slot_name(data[base + 36]), data[base + 36]);
        row("Drive 2 (right)", slot_color(data[base + 38]),
            slot_name(data[base + 38]), data[base + 38]);
        border();
    }

    printf(CYAN "|" RESET DIM "  Auto-refresh: 5s          [R] refresh now          [Q] quit  "
           RESET CYAN "|" RESET "\n");
    border();
    fflush(stdout);
}

static int wait_for_key(int interactive)
{
    fd_set read_set;
    struct timeval timeout;
    char key;
    int result;

    if (!interactive)
        return 'q';

    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    timeout.tv_sec = REFRESH_SECONDS;
    timeout.tv_usec = 0;
    result = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
    if (result > 0 && read(STDIN_FILENO, &key, 1) == 1)
        return key;
    return 'r';
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/sg1";
    unsigned char data[RESPONSE_SIZE];
    char error[256];
    int interactive;
    int key;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [/dev/sgN]\n", argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    atexit(restore_terminal);
    interactive = enable_terminal_mode();

    do {
        error[0] = '\0';
        if (read_raid_status(device, data, error, sizeof(error)) == 0)
            draw_screen(device, data, NULL);
        else
            draw_screen(device, data, error);

        key = wait_for_key(interactive);
        if (key == 'q' || key == 'Q')
            break;
    } while (!stopping);

    return 0;
}
