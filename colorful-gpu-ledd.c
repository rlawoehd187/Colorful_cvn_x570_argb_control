/* Root helper that applies one RGB colour to the GALAX RTX 3070 LED controller.
 *
 * It takes 3-byte {r, g, b} datagrams on a systemd-provided socket, so the
 * unprivileged effects daemon never needs access to the GPU's I2C bus. Only the
 * colour and mode registers of the verified chip are ever written; the save
 * register is not used, so nothing persists on the card.
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_FD 3
#define ADAPTER_PREFIX "NVIDIA i2c adapter 1 at "
#define CHIP_ADDRESS 0x23
#define REG_ID 0x00
#define REG_RED 0x02
#define REG_GREEN 0x03
#define REG_BLUE 0x04
#define REG_MODE_1 0x05
#define REG_MODE_2 0x06
#define CHIP_ID 0x30
#define MODE_STATIC_1 0x00
#define MODE_STATIC_2 0x20
#define MESSAGE_SIZE 3
#define MIN_WRITE_GAP_US 20000
#define MODE_CHECK_INTERVAL_S 5

static int smbus_transfer(int fd, char direction, uint8_t reg, uint8_t *value)
{
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args = {
        .read_write = direction,
        .command = reg,
        .size = I2C_SMBUS_BYTE_DATA,
        .data = &data,
    };

    data.byte = *value;
    if (ioctl(fd, I2C_SMBUS, &args) < 0)
        return -1;
    *value = data.byte;
    return 0;
}

static int read_register(int fd, uint8_t reg, uint8_t *value)
{
    return smbus_transfer(fd, I2C_SMBUS_READ, reg, value);
}

static int write_register(int fd, uint8_t reg, uint8_t value)
{
    int result = smbus_transfer(fd, I2C_SMBUS_WRITE, reg, &value);

    if (result < 0 && (errno == ENODEV || errno == ENXIO)) {
        perror("GPU I2C adapter disappeared");
        exit(EXIT_FAILURE);
    }
    return result;
}

static int find_adapter(char *out, size_t size)
{
    glob_t names;
    int found = 0;

    if (glob("/sys/bus/i2c/devices/i2c-*/name", 0, NULL, &names) != 0)
        return 0;
    for (size_t i = 0; i < names.gl_pathc; i++) {
        char name[64] = "";
        FILE *f = fopen(names.gl_pathv[i], "r");

        if (!f)
            continue;
        if (fgets(name, sizeof name, f) && strncmp(name, ADAPTER_PREFIX, strlen(ADAPTER_PREFIX)) == 0) {
            int bus;

            if (sscanf(names.gl_pathv[i], "/sys/bus/i2c/devices/i2c-%d/", &bus) == 1) {
                snprintf(out, size, "/dev/i2c-%d", bus);
                found++;
            }
        }
        fclose(f);
    }
    globfree(&names);
    return found == 1;
}

static int open_chip(void)
{
    char path[PATH_MAX];
    uint8_t id = 0;

    if (!find_adapter(path, sizeof path)) {
        fprintf(stderr, "GPU I2C adapter not found exactly once\n");
        return -1;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, I2C_SLAVE, CHIP_ADDRESS) < 0 || read_register(fd, REG_ID, &id) < 0) {
        perror(path);
        return -1;
    }
    if (id != CHIP_ID) {
        fprintf(stderr, "unexpected chip id 0x%02x on %s, refusing to write\n", id, path);
        return -1;
    }
    return fd;
}

static void ensure_static_mode(int fd)
{
    uint8_t mode_1 = 0, mode_2 = 0;

    if (read_register(fd, REG_MODE_1, &mode_1) < 0 || read_register(fd, REG_MODE_2, &mode_2) < 0)
        return;
    if (mode_1 != MODE_STATIC_1 || mode_2 != MODE_STATIC_2) {
        write_register(fd, REG_MODE_1, MODE_STATIC_1);
        write_register(fd, REG_MODE_2, MODE_STATIC_2);
    }
}

static void write_color(int fd, const uint8_t color[MESSAGE_SIZE])
{
    write_register(fd, REG_RED, color[0]);
    write_register(fd, REG_GREEN, color[1]);
    write_register(fd, REG_BLUE, color[2]);
}

/* Keeps only the newest pending colour so a backlog never reaches the bus. */
static int receive_latest(int sock, uint8_t color[MESSAGE_SIZE])
{
    int received = 0;

    for (;;) {
        uint8_t message[MESSAGE_SIZE + 1];
        ssize_t size = recv(sock, message, sizeof message, MSG_DONTWAIT);

        if (size < 0)
            return received;
        if (size == MESSAGE_SIZE) {
            memcpy(color, message, MESSAGE_SIZE);
            received = 1;
        }
    }
}

int main(void)
{
    const char *listen_fds = getenv("LISTEN_FDS");
    uint8_t color[MESSAGE_SIZE], applied[MESSAGE_SIZE];
    int have_applied = 0;
    time_t next_mode_check;

    if (!listen_fds || strcmp(listen_fds, "1") != 0) {
        fprintf(stderr, "must be started through colorful-gpu-led.socket\n");
        return EXIT_FAILURE;
    }

    int chip = open_chip();
    if (chip < 0)
        return EXIT_FAILURE;

    ensure_static_mode(chip);
    next_mode_check = time(NULL) + MODE_CHECK_INTERVAL_S;

    for (;;) {
        struct pollfd wait_for = {.fd = LISTEN_FD, .events = POLLIN};

        poll(&wait_for, 1, MODE_CHECK_INTERVAL_S * 1000);

        /* The card can fall back to its stored mode across suspend. */
        if (time(NULL) >= next_mode_check) {
            ensure_static_mode(chip);
            next_mode_check = time(NULL) + MODE_CHECK_INTERVAL_S;
        }

        if (receive_latest(LISTEN_FD, color) && (!have_applied || memcmp(color, applied, MESSAGE_SIZE) != 0)) {
            write_color(chip, color);
            memcpy(applied, color, MESSAGE_SIZE);
            have_applied = 1;
            usleep(MIN_WRITE_GAP_US);
        }
    }
}
