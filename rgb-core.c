/* The controller is a USB HID device (2f4c:1000, usage page 0xff01). One frame
 * is 10 data reports followed by a commit report, each 65 bytes including the
 * leading report ID:
 *
 *   data:   00 01 00 88 <page 0-9> <20 LEDs x 3 colour bytes>
 *   commit: 00 01 00 88 ff <zero padding>
 */
#include "rgb-core.h"

#include <dirent.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define REPORT_SIZE 65
#define PAGES 10
#define LEDS_PER_PAGE 20
#define HEADER_SIZE 4
#define COMMIT_PAGE 0xff

static const uint8_t HEADER[HEADER_SIZE] = {0x00, 0x01, 0x00, 0x88};
static const uint8_t USAGE_PAGE_PREFIX[3] = {0x06, 0x01, 0xff};
static const Rgb WHITE = {255, 255, 255};

const char *const EFFECT_KEYS[EFFECT_COUNT] = {
    "static", "breath", "flicker", "color_cycle", "rainbow",
    "starry", "meteor", "sparkle", "meteor_shower", "lightning", "tune", "sleep", "cpu",
};

const char *const EFFECT_LABELS[EFFECT_COUNT] = {
    "고정", "호흡", "깜빡임", "색상 순환", "무지개",
    "별빛", "별똥별", "샤랄라", "유성비", "번개", "음악 반응", "수면", "CPU 온도",
};

const char *const ORDER_NAMES[ORDER_COUNT] = {"rgb", "grb", "rbg", "gbr", "brg", "bgr"};

const ZoneInfo ZONES[ZONE_COUNT] = {
    {"led", "보드 LED", 18},
    {"12v_1", "12V 헤더 1", 1},
    {"12v_2", "12V 헤더 2", 1},
    {"5v_1", "5V ARGB 1", 60},
    {"5v_2", "5V ARGB 2", 60},
    {"5v_3", "5V ARGB 3", 60},
    {"gpu", "그래픽카드", 1},
};

gboolean effect_uses_color(Effect effect)
{
    return effect != EFFECT_COLOR_CYCLE && effect != EFFECT_RAINBOW && effect != EFFECT_CPU;
}

gboolean effect_uses_speed(Effect effect)
{
    return effect != EFFECT_STATIC && effect != EFFECT_TUNE && effect != EFFECT_CPU;
}

gboolean effect_uses_direction(Effect effect)
{
    return effect == EFFECT_RAINBOW || effect == EFFECT_METEOR || effect == EFFECT_SPARKLE ||
           effect == EFFECT_METEOR_SHOWER;
}

int zone_slot(Zone zone, int index)
{
    switch (zone) {
    case ZONE_LED:
        return 2 + (index + 12) % 18; /* the board's LED ring starts at slot 14 */
    case ZONE_12V_1:
        return 0;
    case ZONE_12V_2:
        return 1;
    case ZONE_5V_1:
        return 20 + index;
    case ZONE_5V_2:
        return 80 + index;
    case ZONE_GPU:
        return GPU_SLOT;
    default:
        return 140 + index;
    }
}

void config_defaults(Config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->enabled = TRUE;
    cfg->effect = EFFECT_STATIC;
    cfg->speed = 3;
    cfg->brightness = 100;
    cfg->sensitivity = 10;
    cfg->cpu_min = 45;
    cfg->cpu_max = 70;
    for (int i = 0; i < EFFECT_COUNT; i++)
        cfg->colors[i] = WHITE;
    cfg->colors[EFFECT_METEOR] = (Rgb){200, 220, 255};
    cfg->colors[EFFECT_SPARKLE] = (Rgb){255, 190, 230};
    cfg->colors[EFFECT_METEOR_SHOWER] = (Rgb){170, 205, 255};
    for (int i = 0; i < ZONE_COUNT; i++) {
        cfg->zones[i].enabled = TRUE;
        cfg->zones[i].length = ZONES[i].max_leds;
        cfg->zones[i].color = WHITE;
    }
}

char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "colorful-rgb", "config.ini", NULL);
}

static int read_int(GKeyFile *kf, const char *group, const char *key, int fallback, int min, int max)
{
    GError *error = NULL;
    int value = g_key_file_get_integer(kf, group, key, &error);

    if (error) {
        g_error_free(error);
        return fallback;
    }
    return CLAMP(value, min, max);
}

static gboolean read_bool(GKeyFile *kf, const char *group, const char *key, gboolean fallback)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(kf, group, key, &error);

    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

static Rgb read_color(GKeyFile *kf, const char *group, const char *key, Rgb fallback)
{
    Rgb color = fallback;
    char *text = g_key_file_get_string(kf, group, key, NULL);

    if (text) {
        unsigned r, g, b;
        if (sscanf(text, "#%2x%2x%2x", &r, &g, &b) == 3)
            color = (Rgb){(uint8_t)r, (uint8_t)g, (uint8_t)b};
        g_free(text);
    }
    return color;
}

static void write_color(GKeyFile *kf, const char *group, const char *key, Rgb color)
{
    char text[8];

    snprintf(text, sizeof text, "#%02x%02x%02x", color.r, color.g, color.b);
    g_key_file_set_string(kf, group, key, text);
}

void config_load(Config *cfg)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();

    config_defaults(cfg);
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *effect = g_key_file_get_string(kf, "general", "effect", NULL);

        for (int i = 0; effect && i < EFFECT_COUNT; i++)
            if (strcmp(effect, EFFECT_KEYS[i]) == 0)
                cfg->effect = (Effect)i;
        g_free(effect);

        cfg->enabled = read_bool(kf, "general", "enabled", cfg->enabled);
        cfg->reverse_direction = read_bool(kf, "general", "reverse_direction", FALSE);
        cfg->speed = read_int(kf, "general", "speed", cfg->speed, 1, 5);
        cfg->brightness = read_int(kf, "general", "brightness", cfg->brightness, 0, 100);
        cfg->sensitivity = read_int(kf, "general", "sensitivity", cfg->sensitivity, 1, 20);
        cfg->cpu_min = read_int(kf, "general", "cpu_min", cfg->cpu_min, 20, 110);
        cfg->cpu_max = read_int(kf, "general", "cpu_max", cfg->cpu_max, 21, 120);
        cfg->order = read_int(kf, "general", "order", 0, 0, ORDER_COUNT - 1);
        if (cfg->cpu_max <= cfg->cpu_min)
            cfg->cpu_max = cfg->cpu_min + 1;

        for (int i = 0; i < EFFECT_COUNT; i++)
            cfg->colors[i] = read_color(kf, "colors", EFFECT_KEYS[i], cfg->colors[i]);

        for (int i = 0; i < ZONE_COUNT; i++) {
            char *group = g_strconcat("zone.", ZONES[i].key, NULL);
            ZoneConfig *zone = &cfg->zones[i];

            zone->enabled = read_bool(kf, group, "enabled", zone->enabled);
            zone->reverse = read_bool(kf, group, "reverse", FALSE);
            zone->use_color = read_bool(kf, group, "use_color", FALSE);
            zone->length = read_int(kf, group, "length", zone->length, 1, ZONES[i].max_leds);
            zone->color = read_color(kf, group, "color", zone->color);
            g_free(group);
        }
    }
    g_free(path);
    g_key_file_unref(kf);
}

gboolean config_save(const Config *cfg, GError **error)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();
    char *dir = g_path_get_dirname(path);

    g_key_file_set_string(kf, "general", "effect", EFFECT_KEYS[cfg->effect]);
    g_key_file_set_boolean(kf, "general", "enabled", cfg->enabled);
    g_key_file_set_boolean(kf, "general", "reverse_direction", cfg->reverse_direction);
    g_key_file_set_integer(kf, "general", "speed", cfg->speed);
    g_key_file_set_integer(kf, "general", "brightness", cfg->brightness);
    g_key_file_set_integer(kf, "general", "sensitivity", cfg->sensitivity);
    g_key_file_set_integer(kf, "general", "cpu_min", cfg->cpu_min);
    g_key_file_set_integer(kf, "general", "cpu_max", cfg->cpu_max);
    g_key_file_set_integer(kf, "general", "order", cfg->order);

    for (int i = 0; i < EFFECT_COUNT; i++)
        write_color(kf, "colors", EFFECT_KEYS[i], cfg->colors[i]);

    for (int i = 0; i < ZONE_COUNT; i++) {
        char *group = g_strconcat("zone.", ZONES[i].key, NULL);
        const ZoneConfig *zone = &cfg->zones[i];

        g_key_file_set_boolean(kf, group, "enabled", zone->enabled);
        g_key_file_set_boolean(kf, group, "reverse", zone->reverse);
        g_key_file_set_boolean(kf, group, "use_color", zone->use_color);
        g_key_file_set_integer(kf, group, "length", zone->length);
        write_color(kf, group, "color", zone->color);
        g_free(group);
    }

    g_mkdir_with_parents(dir, 0755);
    gboolean ok = g_key_file_save_to_file(kf, path, error);
    g_free(dir);
    g_free(path);
    g_key_file_unref(kf);
    return ok;
}

static gboolean is_control_interface(const char *hid_path)
{
    char path[PATH_MAX];
    uint8_t prefix[sizeof USAGE_PAGE_PREFIX];

    snprintf(path, sizeof path, "%s/report_descriptor", hid_path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return FALSE;
    size_t got = fread(prefix, 1, sizeof prefix, f);
    fclose(f);
    return got == sizeof prefix && memcmp(prefix, USAGE_PAGE_PREFIX, sizeof prefix) == 0;
}

gboolean device_find(char *out, size_t size)
{
    glob_t matches;
    gboolean found = FALSE;

    if (glob("/sys/bus/hid/devices/*:2F4C:1000.*", 0, NULL, &matches) != 0)
        return FALSE;

    for (size_t i = 0; i < matches.gl_pathc && !found; i++) {
        char path[PATH_MAX];

        if (!is_control_interface(matches.gl_pathv[i]))
            continue;
        snprintf(path, sizeof path, "%s/hidraw", matches.gl_pathv[i]);
        DIR *dir = opendir(path);
        if (!dir)
            continue;
        for (struct dirent *e; !found && (e = readdir(dir));) {
            if (strncmp(e->d_name, "hidraw", 6) == 0) {
                snprintf(out, size, "/dev/%s", e->d_name);
                found = TRUE;
            }
        }
        closedir(dir);
    }
    globfree(&matches);
    return found;
}

gboolean device_send(int fd, const Rgb frame[SLOT_COUNT], int order)
{
    const char *channels = ORDER_NAMES[order];
    uint8_t report[REPORT_SIZE];

    for (int page = 0; page < PAGES; page++) {
        memset(report, 0, sizeof report);
        memcpy(report, HEADER, HEADER_SIZE);
        report[HEADER_SIZE] = (uint8_t)page;

        for (int led = 0; led < LEDS_PER_PAGE; led++) {
            const Rgb *color = &frame[page * LEDS_PER_PAGE + led];
            const uint8_t values[3] = {color->r, color->g, color->b};

            for (int c = 0; c < 3; c++) {
                int channel = (int)(strchr("rgb", channels[c]) - "rgb");
                report[HEADER_SIZE + 1 + led * 3 + c] = values[channel];
            }
        }
        if (write(fd, report, sizeof report) != (ssize_t)sizeof report)
            return FALSE;
    }

    memset(report, 0, sizeof report);
    memcpy(report, HEADER, HEADER_SIZE);
    report[HEADER_SIZE] = COMMIT_PAGE;
    return write(fd, report, sizeof report) == (ssize_t)sizeof report;
}
