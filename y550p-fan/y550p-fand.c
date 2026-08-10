#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PROGRAM_NAME "y550p-fand"
#define PROGRAM_VERSION "1.0.0"
#define RUNTIME_DIR "/run/y550p-fan"
#define LOCK_PATH RUNTIME_DIR "/lock"
#define ORIGINAL_PATH RUNTIME_DIR "/original-f502"
#define STATUS_PATH RUNTIME_DIR "/status"
#define STATUS_TMP_PATH RUNTIME_DIR "/status.tmp"

/* Lenovo IdeaPad Y550P / Compal NIWBA LA-5371P / ENE KB926D. */
enum {
    EC_IDX_HI = 0xff29,
    EC_IDX_LO = 0xff2a,
    EC_IDX_DATA = 0xff2b,

    EC_F502 = 0xf502,       /* copied by stock EC firmware to DAC1 */
    EC_F60B = 0xf60b,       /* stock manual-path gate, bit 0 */
    EC_F6B1 = 0xf6b1,       /* system-state gate, bit 0 */
    EC_F78E = 0xf78e,       /* manual-path state */
    EC_FB31 = 0xfb31,       /* stock firmware fan-output cache */
    EC_FC2F = 0xfc2f,       /* SUSP# input, bit 5 */
    EC_DAC1 = 0xff11,

    EC_FANCFG0 = 0xfe20,
    EC_FANSTS0 = 0xfe21,
    EC_FANMONH0 = 0xfe22,
    EC_FANMONL0 = 0xfe23,
};

enum {
    CMD_LOW = 0xd0,
    CMD_MEDIUM = 0xe0,
    CMD_HIGH = 0xf0,
    CMD_MAX = 0xff,
    START_KICK_SECONDS = 5,
    TACH_RETRY_SECONDS = 5,
    LOOP_MS = 500,
    LOG_INTERVAL_LOOPS = 120,
};

static volatile sig_atomic_t stop_requested;
static uint8_t original_f502;
static int have_io;
static int have_original;
static int lock_fd = -1;

static uint8_t ec_read(uint16_t reg)
{
    outb((uint8_t)(reg >> 8), EC_IDX_HI);
    outb((uint8_t)reg, EC_IDX_LO);
    return inb(EC_IDX_DATA);
}

static void ec_write(uint16_t reg, uint8_t value)
{
    outb((uint8_t)(reg >> 8), EC_IDX_HI);
    outb((uint8_t)reg, EC_IDX_LO);
    outb(value, EC_IDX_DATA);
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_line(const char *level, const char *fmt, ...)
{
    va_list ap;
    fprintf(stdout, "%s ", level);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t got = read(fd, buf, size - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (got <= 0)
        return -1;
    buf[got] = '\0';
    while (got > 0 && isspace((unsigned char)buf[got - 1]))
        buf[--got] = '\0';
    return 0;
}

static bool supported_machine(void)
{
    char vendor[128];
    char product[128];
    char version[128];

    if (read_text("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor)) != 0 ||
        read_text("/sys/class/dmi/id/product_name", product,
                  sizeof(product)) != 0)
        return false;

    if (strcmp(vendor, "LENOVO") != 0)
        return false;
    if (strcmp(product, "20035") == 0)
        return true;

    return read_text("/sys/class/dmi/id/product_version", version,
                     sizeof(version)) == 0 &&
           strstr(version, "IdeaPad Y550P") != NULL;
}

static int acquire_instance_lock(void)
{
    if (mkdir(RUNTIME_DIR, 0755) != 0 && errno != EEXIST)
        return -1;

    lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0)
        return -1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
        return -1;
    return 0;
}

static int save_or_load_original_f502(uint8_t current)
{
    int fd = open(ORIGINAL_PATH,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd >= 0) {
        char value[8];
        int length = snprintf(value, sizeof(value), "%02x\n", current);
        ssize_t written = write(fd, value, (size_t)length);
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        if (written != length) {
            if (written >= 0)
                errno = EIO;
            (void)unlink(ORIGINAL_PATH);
            return -1;
        }
        original_f502 = current;
        return 0;
    }
    if (errno != EEXIST)
        return -1;

    char value[32];
    if (read_text(ORIGINAL_PATH, value, sizeof(value)) != 0)
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 16);
    if (errno != 0 || end == value || *end != '\0' || parsed > 0xff)
        return -1;
    original_f502 = (uint8_t)parsed;
    return 0;
}

static bool wanted_hwmon(const char *name)
{
    return strcmp(name, "coretemp") == 0 ||
           strcmp(name, "nouveau") == 0 ||
           strcmp(name, "acpitz") == 0;
}

/* Return the maximum CPU/GPU/ACPI temperature in millidegrees Celsius. */
static long max_temperature_mc(void)
{
    DIR *base = opendir("/sys/class/hwmon");
    if (!base)
        return -1;

    long maximum = -1;
    struct dirent *hw;
    while ((hw = readdir(base)) != NULL) {
        if (strncmp(hw->d_name, "hwmon", 5) != 0)
            continue;

        char dirpath[PATH_MAX];
        char path[PATH_MAX];
        char name[128];
        if (snprintf(dirpath, sizeof(dirpath), "/sys/class/hwmon/%s",
                     hw->d_name) >= (int)sizeof(dirpath))
            continue;
        if (snprintf(path, sizeof(path), "%s/name", dirpath) >=
            (int)sizeof(path))
            continue;
        if (read_text(path, name, sizeof(name)) != 0 || !wanted_hwmon(name))
            continue;

        DIR *sensor_dir = opendir(dirpath);
        if (!sensor_dir)
            continue;
        struct dirent *entry;
        while ((entry = readdir(sensor_dir)) != NULL) {
            unsigned index;
            char expected[64];
            if (sscanf(entry->d_name, "temp%u", &index) != 1)
                continue;
            if (snprintf(expected, sizeof(expected), "temp%u_input", index) >=
                    (int)sizeof(expected) ||
                strcmp(entry->d_name, expected) != 0)
                continue;
            if (snprintf(path, sizeof(path), "%s/%s", dirpath,
                         entry->d_name) >= (int)sizeof(path))
                continue;
            char value[64];
            if (read_text(path, value, sizeof(value)) != 0)
                continue;
            char *end = NULL;
            errno = 0;
            long parsed = strtol(value, &end, 10);
            if (errno == 0 && end != value && parsed > 0 && parsed < 150000 &&
                parsed > maximum)
                maximum = parsed;
        }
        closedir(sensor_dir);
    }
    closedir(base);
    return maximum;
}

static uint16_t read_tach(void)
{
    return (uint16_t)(((ec_read(EC_FANMONH0) & 0x0f) << 8) |
                      ec_read(EC_FANMONL0));
}

static unsigned estimated_rpm(uint16_t count)
{
    if (count == 0 || count == 0x0fff)
        return 0;
    return 468750u / count;
}

struct tach_sample {
    uint8_t status;
    uint16_t count;
    unsigned rpm;
    bool valid;
};

static struct tach_sample sample_tach(void)
{
    struct tach_sample sample;
    sample.status = ec_read(EC_FANSTS0);
    sample.count = read_tach();
    /* Counts below 0x40 would imply an impossible >7300 RPM transition. */
    sample.valid = (sample.status & 0x01) && !(sample.status & 0x02) &&
                   sample.count >= 0x0040 && sample.count != 0x0fff;
    sample.rpm = sample.valid ? estimated_rpm(sample.count) : 0;

    /* Bits 1:0 are write-one-to-clear timeout/update flags. */
    if (sample.status & 0x03)
        ec_write(EC_FANSTS0, sample.status & 0x03);
    return sample;
}

static uint8_t command_for_temperature(long temp_mc)
{
    /* A missing sensor is a fault: fail at full cooling. */
    if (temp_mc < 0 || temp_mc >= 75000)
        return CMD_MAX;
    if (temp_mc >= 65000)
        return CMD_HIGH;
    if (temp_mc >= 55000)
        return CMD_MEDIUM;
    return CMD_LOW;
}

static bool stock_path_ready(void)
{
    uint8_t f78e = ec_read(EC_F78E);
    return (ec_read(EC_F60B) & 0x01) && (ec_read(EC_F6B1) & 0x01) &&
           (ec_read(EC_FC2F) & 0x20) && (f78e & 0x03) == 0x03 &&
           !(f78e & 0x14);
}

static void write_status_file(long temp_mc, uint8_t wanted, uint8_t sent,
                              const struct tach_sample *tach,
                              bool kicking, bool path_ready)
{
    FILE *fp = fopen(STATUS_TMP_PATH, "w");
    if (!fp)
        return;
    if (temp_mc >= 0)
        fprintf(fp, "temperature_c=%.1f\n", (double)temp_mc / 1000.0);
    else
        fprintf(fp, "temperature_c=unavailable\n");
    fprintf(fp, "wanted_command=0x%02x\n", wanted);
    fprintf(fp, "sent_command=0x%02x\n", sent);
    fprintf(fp, "effective_command=0x%02x\n", ec_read(EC_DAC1));
    fprintf(fp, "tach_valid=%s\n", tach->valid ? "yes" : "no");
    fprintf(fp, "rpm=%u\n", tach->rpm);
    fprintf(fp, "tach_status=0x%02x\n", tach->status);
    fprintf(fp, "tach_count=0x%03x\n", tach->count);
    fprintf(fp, "startup_or_retry_kick=%s\n", kicking ? "yes" : "no");
    fprintf(fp, "stock_path_ready=%s\n", path_ready ? "yes" : "no");
    if (fclose(fp) == 0)
        (void)rename(STATUS_TMP_PATH, STATUS_PATH);
}

static void handle_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void restore_original(void)
{
    if (have_io && have_original) {
        ec_write(EC_F502, original_f502);
        have_original = 0;
        (void)unlink(ORIGINAL_PATH);
    }
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: %s [--help | --version]\n"
            "Model-specific EC fan controller for Lenovo IdeaPad Y550P.\n",
            PROGRAM_NAME);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
        return 0;
    }
    if (argc != 1) {
        print_usage(stderr);
        return 2;
    }
    if (!supported_machine()) {
        fprintf(stderr,
                "%s: unsupported machine; expected Lenovo IdeaPad Y550P "
                "(type 20035)\n",
                PROGRAM_NAME);
        return 1;
    }
    if (acquire_instance_lock() != 0) {
        fprintf(stderr, "%s: cannot acquire %s: %s\n", PROGRAM_NAME,
                LOCK_PATH, strerror(errno));
        return 1;
    }
    if (ioperm(EC_IDX_HI, 3, 1) != 0) {
        perror("ioperm");
        return 1;
    }
    have_io = 1;
    if (save_or_load_original_f502(ec_read(EC_F502)) != 0) {
        fprintf(stderr, "%s: cannot preserve original F502: %s\n",
                PROGRAM_NAME, strerror(errno));
        ioperm(EC_IDX_HI, 3, 0);
        have_io = 0;
        return 1;
    }
    have_original = 1;
    atexit(restore_original);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    if (ec_read(EC_FANCFG0) != 0x81)
        log_line("WARNING", "unexpected tach configuration FE20=0x%02x",
                 ec_read(EC_FANCFG0));

    long long kick_until = monotonic_ms() + START_KICK_SECONDS * 1000;
    unsigned invalid_samples = 0;
    unsigned loops = 0;
    uint8_t previous_sent = 0;
    log_line("INFO", "started original_f502=0x%02x startup_kick=%ds",
             original_f502, START_KICK_SECONDS);

    /* Discard stale timeout/update state before starting the kick. */
    ec_write(EC_FANSTS0, 0x03);

    while (!stop_requested) {
        long temp_mc = max_temperature_mc();
        uint8_t wanted = command_for_temperature(temp_mc);
        long long now = monotonic_ms();
        bool kicking = now < kick_until;
        uint8_t sent = kicking ? CMD_MAX : wanted;
        bool path_ready = stock_path_ready();

        /* The exact stock firmware copies this byte to FB31 and DAC1. */
        ec_write(EC_F502, sent);
        usleep(20 * 1000);

        struct tach_sample tach = sample_tach();
        if (tach.valid) {
            invalid_samples = 0;
        } else {
            ++invalid_samples;
        }

        /* Re-kick at full command when a running fan loses its tach signal. */
        if (!kicking && invalid_samples >= 4) {
            kick_until = now + TACH_RETRY_SECONDS * 1000;
            invalid_samples = 0;
            kicking = true;
            sent = CMD_MAX;
            ec_write(EC_F502, sent);
            log_line("WARNING", "tach lost; starting %ds full-speed retry",
                     TACH_RETRY_SECONDS);
        }

        if (sent != previous_sent || loops % LOG_INTERVAL_LOOPS == 0 ||
            !path_ready ||
            (!tach.valid && loops % 4 == 0)) {
            if (temp_mc >= 0)
                log_line(tach.valid ? "INFO" : "WARNING",
                         "temp=%.1fC wanted=0x%02x sent=0x%02x "
                         "effective=0x%02x tach=%s rpm=%u status=0x%02x "
                         "count=0x%03x path=%s",
                         (double)temp_mc / 1000.0, wanted, sent,
                         ec_read(EC_DAC1),
                         tach.valid ? "valid" : "invalid", tach.rpm,
                         tach.status, tach.count,
                         path_ready ? "ready" : "not-ready");
            else
                log_line("ERROR",
                         "temperature unavailable; sent=0x%02x "
                         "effective=0x%02x tach=%s rpm=%u path=%s",
                         sent, ec_read(EC_DAC1),
                         tach.valid ? "valid" : "invalid", tach.rpm,
                         path_ready ? "ready" : "not-ready");
        }
        write_status_file(temp_mc, wanted, sent, &tach, kicking, path_ready);
        previous_sent = sent;
        ++loops;

        struct timespec delay = {
            .tv_sec = LOOP_MS / 1000,
            .tv_nsec = (LOOP_MS % 1000) * 1000 * 1000,
        };
        nanosleep(&delay, NULL);
    }

    log_line("INFO", "stopping; restoring F502=0x%02x", original_f502);
    restore_original();
    (void)unlink(STATUS_PATH);
    ioperm(EC_IDX_HI, 3, 0);
    have_io = 0;
    if (lock_fd >= 0)
        close(lock_fd);
    return 0;
}
