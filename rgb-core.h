/* Shared configuration and device access for the Colorful X570M RGB tools. */
#ifndef RGB_CORE_H
#define RGB_CORE_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

#define BOARD_SLOT_COUNT 200
#define GPU_SLOT BOARD_SLOT_COUNT
#define SLOT_COUNT (BOARD_SLOT_COUNT + 1)
#define ORDER_COUNT 6

typedef struct {
    uint8_t r, g, b;
} Rgb;

typedef enum {
    EFFECT_STATIC,
    EFFECT_BREATH,
    EFFECT_FLICKER,
    EFFECT_COLOR_CYCLE,
    EFFECT_RAINBOW,
    EFFECT_STARRY,
    EFFECT_METEOR,
    EFFECT_SPARKLE,
    EFFECT_METEOR_SHOWER,
    EFFECT_LIGHTNING,
    EFFECT_TUNE,
    EFFECT_SLEEP,
    EFFECT_CPU,
    EFFECT_COUNT
} Effect;

typedef enum { ZONE_LED, ZONE_12V_1, ZONE_12V_2, ZONE_5V_1, ZONE_5V_2, ZONE_5V_3, ZONE_GPU, ZONE_COUNT } Zone;

typedef struct {
    const char *key;
    const char *label;
    int max_leds;
} ZoneInfo;

typedef struct {
    gboolean enabled;
    gboolean reverse;
    gboolean use_color;
    int length;
    Rgb color;
} ZoneConfig;

typedef struct {
    gboolean enabled;
    gboolean reverse_direction;
    Effect effect;
    int speed;
    int brightness;
    int sensitivity;
    int cpu_min;
    int cpu_max;
    int order;
    Rgb colors[EFFECT_COUNT];
    ZoneConfig zones[ZONE_COUNT];
} Config;

extern const char *const EFFECT_KEYS[EFFECT_COUNT];
extern const char *const EFFECT_LABELS[EFFECT_COUNT];
extern const char *const ORDER_NAMES[ORDER_COUNT];
extern const ZoneInfo ZONES[ZONE_COUNT];

gboolean effect_uses_color(Effect effect);
gboolean effect_uses_speed(Effect effect);
gboolean effect_uses_direction(Effect effect);

int zone_slot(Zone zone, int index);

void config_defaults(Config *cfg);
char *config_path(void);
void config_load(Config *cfg);
gboolean config_save(const Config *cfg, GError **error);

gboolean device_find(char *out, size_t size);
gboolean device_send(int fd, const Rgb frame[SLOT_COUNT], int order);

#endif
