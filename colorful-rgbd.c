/* Renders LED effects and streams them to the controller. Reloads the
 * configuration whenever the GUI saves it and reconnects after suspend or
 * device loss. */
#include "rgb-core.h"

#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FRAME_SECONDS (1.0 / 30.0)
#define RELOAD_INTERVAL 0.1
#define RECONNECT_INTERVAL 1.0
#define RESEND_INTERVAL 1.0
#define CPU_READ_INTERVAL 1.0
#define AUDIO_RATE 22050
#define AUDIO_SAMPLES 512
#define METEOR_TAIL 0.45    /* tail length, as a fraction of the zone */
#define METEOR_PEAK 0.9
#define METEOR_FADE_IN 0.1  /* head travel over which the meteor appears */
#define METEOR_SOLO_ATTACK 0.05 /* head travel over which a lone LED lights up */
#define GPU_METEOR_ATTACK 0.01  /* about one frame, so the card snaps to full */
#define GPU_SOCKET "/run/colorful-gpu-led.sock"
#define GPU_SEND_INTERVAL 0.02 /* below FRAME_SECONDS, so every frame goes out */
#define GPU_RESEND_INTERVAL 1.0
#define GPU_BREATH_FLOOR 0.3 /* the card's LEDs look dim, so it never fades out */

typedef struct {
    int fd;
    Rgb last;
    double last_sent;
    gboolean sent;
} GpuLink;

#define SPARK_COUNT 8
#define SPARK_TAIL 0.14     /* tail length, as a fraction of the zone */
#define SPARK_WHITEN 0.7    /* how far a glint shifts the colour toward white */
#define SPARK_SOLO_FLOOR 0.4 /* a lone LED shimmers but never goes dim */

typedef struct {
    double position;
    double fall_rate;
    double twinkle_hz;
    double phase;
} Spark;

#define STREAK_COUNT 5
#define STREAK_WHITEN 0.8     /* how white the head burns */
#define STREAK_FADE_IN 0.05   /* travel over which a streak appears */
#define STREAK_SOLO_FLOOR 0.3 /* a lone LED flashes but never goes dim */
#define GPU_STREAK_SHARPNESS 14.0 /* narrows each swell so the card falls to black between */

typedef struct {
    double position;
    double fall_rate;
    double tail;
    double peak;
    double wait;
} Streak;

typedef struct {
    Streak streaks[ZONE_COUNT][STREAK_COUNT];
    Spark sparks[ZONE_COUNT][SPARK_COUNT];
    float star[SLOT_COUNT];
    double meteor_head[ZONE_COUNT];
    double meteor_wait[ZONE_COUNT];
    double flash;
    double next_flash;
    double second_flash;
    double audio_level;
    double cpu_temp;
    double next_cpu_read;
} EffectState;

static volatile sig_atomic_t stop_requested;
static gint audio_running;
static gint audio_level_milli;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static Rgb scale(Rgb color, double value)
{
    value = CLAMP(value, 0.0, 1.0);
    return (Rgb){(uint8_t)(color.r * value + 0.5), (uint8_t)(color.g * value + 0.5),
                 (uint8_t)(color.b * value + 0.5)};
}

static Rgb hue_color(double hue)
{
    double h = fmod(fmod(hue, 1.0) + 1.0, 1.0) * 6.0;
    double x = 1.0 - fabs(fmod(h, 2.0) - 1.0);
    double r = 0, g = 0, b = 0;

    switch ((int)h) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
    }
    return (Rgb){(uint8_t)(r * 255 + 0.5), (uint8_t)(g * 255 + 0.5), (uint8_t)(b * 255 + 0.5)};
}

static double read_cpu_temperature(void)
{
    glob_t names;
    double temperature = 0;

    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &names) != 0)
        return 0;
    for (size_t i = 0; i < names.gl_pathc && temperature == 0; i++) {
        char name[32] = "", path[PATH_MAX];
        FILE *f = fopen(names.gl_pathv[i], "r");

        if (!f)
            continue;
        int matched = fscanf(f, "%31s", name);
        fclose(f);
        if (matched != 1 || strcmp(name, "k10temp") != 0)
            continue;

        g_strlcpy(path, names.gl_pathv[i], sizeof path);
        *strrchr(path, '/') = '\0';
        g_strlcat(path, "/temp1_input", sizeof path);
        if ((f = fopen(path, "r"))) {
            int millidegrees;
            if (fscanf(f, "%d", &millidegrees) == 1)
                temperature = millidegrees / 1000.0;
            fclose(f);
        }
    }
    globfree(&names);
    return temperature;
}

/* Captures whatever the default output is playing and publishes its level. */
static gpointer audio_thread(gpointer unused)
{
    static const pa_sample_spec spec = {PA_SAMPLE_FLOAT32NE, AUDIO_RATE, 1};
    const pa_buffer_attr attr = {(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1,
                                 AUDIO_SAMPLES * sizeof(float)};

    (void)unused;
    while (g_atomic_int_get(&audio_running)) {
        int error;
        pa_simple *stream = pa_simple_new(NULL, "colorful-rgbd", PA_STREAM_RECORD, "@DEFAULT_MONITOR@",
                                          "LED music level", &spec, NULL, &attr, &error);

        if (!stream) {
            for (int i = 0; i < 20 && g_atomic_int_get(&audio_running); i++)
                g_usleep(100000);
            continue;
        }
        while (g_atomic_int_get(&audio_running)) {
            float samples[AUDIO_SAMPLES];
            double sum = 0;

            if (pa_simple_read(stream, samples, sizeof samples, &error) < 0)
                break;
            for (int i = 0; i < AUDIO_SAMPLES; i++)
                sum += samples[i] * samples[i];
            g_atomic_int_set(&audio_level_milli, (gint)(sqrt(sum / AUDIO_SAMPLES) * 1000));
        }
        pa_simple_free(stream);
    }
    g_atomic_int_set(&audio_level_milli, 0);
    return NULL;
}

static GThread *audio_update(GThread *thread, gboolean wanted)
{
    if (wanted && !thread) {
        g_atomic_int_set(&audio_running, 1);
        return g_thread_new("audio", audio_thread, NULL);
    }
    if (!wanted && thread) {
        g_atomic_int_set(&audio_running, 0);
        g_thread_join(thread);
        return NULL;
    }
    return thread;
}

static void respawn_spark(Spark *spark, double position)
{
    spark->position = position;
    spark->fall_rate = g_random_double_range(0.06, 0.14);
    spark->twinkle_hz = g_random_double_range(2.0, 5.0);
    spark->phase = g_random_double_range(0.0, 2.0 * G_PI);
}

/* The headers and the GPU fall together so fans and the card light as one. */
static int meteor_clock(int zone)
{
    return zone == ZONE_LED ? ZONE_LED : ZONE_5V_1;
}

static void respawn_streak(Streak *streak, double speed)
{
    streak->position = 0;
    streak->fall_rate = g_random_double_range(0.12, 0.30);
    streak->tail = g_random_double_range(0.25, 0.60);
    streak->peak = g_random_double_range(0.55, 1.0);
    streak->wait = g_random_double_range(0.0, 3.0) / speed;
}

static void update_state(const Config *cfg, EffectState *st, double t, double dt)
{
    double speed = cfg->speed;

    if (cfg->effect == EFFECT_STARRY) {
        for (int i = 0; i < SLOT_COUNT; i++) {
            st->star[i] *= (float)exp(-dt * 1.5);
            if (g_random_double() < dt * 0.25 * speed)
                st->star[i] = 1.0f;
        }
    } else if (cfg->effect == EFFECT_METEOR) {
        for (int z = 0; z < ZONE_COUNT; z++) {
            if (meteor_clock(z) != z)
                continue;
            if (st->meteor_wait[z] > 0) {
                st->meteor_wait[z] -= dt;
                continue;
            }
            st->meteor_head[z] += dt * 0.16 * speed;
            if (st->meteor_head[z] > 1.0 + METEOR_TAIL) {
                st->meteor_head[z] = 0;
                st->meteor_wait[z] = g_random_double_range(0.3, 2.4) / speed;
            }
        }
    } else if (cfg->effect == EFFECT_SPARKLE) {
        for (int z = 0; z < ZONE_COUNT; z++) {
            for (int i = 0; i < SPARK_COUNT; i++) {
                Spark *spark = &st->sparks[z][i];

                if (spark->fall_rate <= 0)
                    respawn_spark(spark, g_random_double_range(-0.8, 1.0));
                spark->position += dt * spark->fall_rate * speed;
                if (spark->position > 1.0 + SPARK_TAIL)
                    respawn_spark(spark, -g_random_double_range(0.0, 0.5));
            }
        }
    } else if (cfg->effect == EFFECT_METEOR_SHOWER) {
        for (int z = 0; z < ZONE_COUNT; z++) {
            for (int i = 0; i < STREAK_COUNT; i++) {
                Streak *streak = &st->streaks[z][i];

                if (streak->fall_rate <= 0)
                    respawn_streak(streak, speed);
                if (streak->wait > 0) {
                    streak->wait -= dt;
                    continue;
                }
                streak->position += dt * streak->fall_rate * speed;
                if (streak->position > 1.0 + streak->tail)
                    respawn_streak(streak, speed);
            }
        }
    } else if (cfg->effect == EFFECT_LIGHTNING) {
        st->flash *= exp(-dt * 12.0);
        if (st->second_flash > 0 && t >= st->second_flash) {
            st->flash = 1.0;
            st->second_flash = 0;
        }
        if (t >= st->next_flash) {
            st->flash = 1.0;
            st->next_flash = t + g_random_double_range(0.6, 5.0) / speed;
            if (g_random_double() < 0.4)
                st->second_flash = t + 0.12;
        }
    } else if (cfg->effect == EFFECT_TUNE) {
        double target = g_atomic_int_get(&audio_level_milli) / 1000.0 * cfg->sensitivity * 0.5;

        target = MIN(target, 1.0);
        st->audio_level += (target - st->audio_level) * MIN(dt * (target > st->audio_level ? 30.0 : 6.0), 1.0);
    } else if (cfg->effect == EFFECT_CPU && t >= st->next_cpu_read) {
        st->cpu_temp = read_cpu_temperature();
        st->next_cpu_read = t + CPU_READ_INTERVAL;
    }
}

static Rgb whiten(Rgb color, double amount)
{
    return (Rgb){(uint8_t)(color.r + (255 - color.r) * amount + 0.5),
                 (uint8_t)(color.g + (255 - color.g) * amount + 0.5),
                 (uint8_t)(color.b + (255 - color.b) * amount + 0.5)};
}

/* Falling glitter: each spark has a short tail and glints at its own rhythm. */
static Rgb sparkle_color(const EffectState *st, int zone, double position, int length, Rgb color,
                         double t, double speed)
{
    double level = 0, shine = 0;

    if (length == 1) {
        double rate = 0.5 + 0.25 * speed;
        double glint = (sin(2.0 * G_PI * 1.3 * rate * t) + sin(2.0 * G_PI * 3.1 * rate * t + 1.0) + 2.0) / 4.0;

        glint *= glint;
        level = SPARK_SOLO_FLOOR + (1.0 - SPARK_SOLO_FLOOR) * glint;
        shine = glint;
    } else {
        double edge = 1.0 / length;

        for (int i = 0; i < SPARK_COUNT; i++) {
            const Spark *spark = &st->sparks[zone][i];
            double distance = spark->position - position;

            if (distance >= SPARK_TAIL || distance <= -edge)
                continue;

            double body = distance < 0 ? 1.0 + distance / edge : pow(1.0 - distance / SPARK_TAIL, 2.0);
            double glint = pow(0.5 + 0.5 * sin(spark->phase + 2.0 * G_PI * spark->twinkle_hz * t), 3.0);

            level += body * (0.35 + 0.65 * glint);
            shine += body * glint;
        }
    }
    return scale(whiten(color, MIN(shine, 1.0) * SPARK_WHITEN), level);
}

/* Several streaks of differing speed, length and brightness falling at once. */
static Rgb shower_color(const EffectState *st, int zone, double position, int length, Rgb color)
{
    if (zone == ZONE_GPU) {
        double strongest = 0;

        for (int i = 0; i < STREAK_COUNT; i++) {
            const Streak *streak = &st->streaks[zone][i];

            if (streak->wait <= 0) {
                double wave = sin(G_PI * MIN(streak->position / (1.0 + streak->tail), 1.0));

                strongest = MAX(strongest, streak->peak * pow(wave, GPU_STREAK_SHARPNESS));
            }
        }
        return scale(whiten(color, strongest * strongest * STREAK_WHITEN), strongest);
    }

    double edge = 1.0 / length;
    double level = 0, shine = 0;

    for (int i = 0; i < STREAK_COUNT; i++) {
        const Streak *streak = &st->streaks[zone][i];

        if (streak->wait > 0)
            continue;

        if (length == 1) {
            double wave = sin(G_PI * MIN(streak->position / (1.0 + streak->tail), 1.0));

            level += streak->peak * wave * wave;
            shine += pow(wave, 8.0);
            continue;
        }

        double distance = streak->position - position;

        if (distance >= streak->tail || distance <= -edge)
            continue;

        double appear = MIN(streak->position / STREAK_FADE_IN, 1.0);
        double body = distance < 0 ? 1.0 + distance / edge : pow(1.0 - distance / streak->tail, 2.4);

        level += streak->peak * appear * body;
        shine += appear * pow(body, 3.0);
    }

    if (length == 1)
        level = STREAK_SOLO_FLOOR + (1.0 - STREAK_SOLO_FLOOR) * MIN(level, 1.0);
    return scale(whiten(color, MIN(shine, 1.0) * STREAK_WHITEN), level);
}

/* Snaps to full brightness as the meteor starts, then fades with its tail. */
static double gpu_meteor_brightness(double head)
{
    double span = 1.0 + METEOR_TAIL - GPU_METEOR_ATTACK;

    if (head < GPU_METEOR_ATTACK)
        return head / GPU_METEOR_ATTACK;
    return pow(MAX(1.0 - (head - GPU_METEOR_ATTACK) / span, 0.0), 1.5);
}

/* A soft head trailing a long fading tail, antialiased across one LED. */
static double meteor_brightness(double head, double position, int length)
{
    double distance = head - position;
    double appear = MIN(head / METEOR_FADE_IN, 1.0);
    double edge = 1.0 / length;

    if (length == 1) { /* a lone LED lights on arrival and fades with the tail */
        double remaining = MAX(1.0 - head / (1.0 + METEOR_TAIL), 0.0);

        return MIN(head / METEOR_SOLO_ATTACK, 1.0) * pow(remaining, 1.5);
    }
    if (distance >= METEOR_TAIL || distance <= -edge)
        return 0;
    if (distance < 0)
        return METEOR_PEAK * appear * (1.0 + distance / edge);
    return METEOR_PEAK * appear * pow(1.0 - distance / METEOR_TAIL, 2.2);
}

static Rgb render_led(const Config *cfg, const EffectState *st, const ZoneConfig *zone, int slot,
                      int index, double t)
{
    Rgb color = zone->use_color ? zone->color : cfg->colors[cfg->effect];
    double position = (double)index / zone->length;
    double speed = cfg->speed;

    switch (cfg->effect) {
    case EFFECT_BREATH: {
        double wave = (1.0 - cos(2.0 * G_PI * t * 0.125 * speed)) / 2.0;

        if (zone == &cfg->zones[ZONE_GPU])
            return scale(color, GPU_BREATH_FLOOR + (1.0 - GPU_BREATH_FLOOR) * wave);
        return scale(color, wave * wave);
    }
    case EFFECT_FLICKER:
        return fmod(t * speed, 1.0) < 0.5 ? color : (Rgb){0, 0, 0};
    case EFFECT_COLOR_CYCLE:
        return hue_color(t * 0.03 * speed);
    case EFFECT_RAINBOW: {
        double direction = cfg->reverse_direction ? -1.0 : 1.0;
        return hue_color(direction * (position - t * 0.1 * speed));
    }
    case EFFECT_STARRY:
        return scale(color, st->star[slot]);
    case EFFECT_METEOR: {
        int z = meteor_clock((int)(zone - cfg->zones));

        if (st->meteor_wait[z] > 0)
            return (Rgb){0, 0, 0};
        if (zone == &cfg->zones[ZONE_GPU])
            return scale(color, gpu_meteor_brightness(st->meteor_head[z]));
        return scale(color, meteor_brightness(st->meteor_head[z],
                                              cfg->reverse_direction ? 1.0 - position : position,
                                              zone->length));
    }
    case EFFECT_SPARKLE:
        return sparkle_color(st, (int)(zone - cfg->zones),
                             cfg->reverse_direction ? 1.0 - position : position, zone->length, color, t, speed);
    case EFFECT_METEOR_SHOWER:
        return shower_color(st, (int)(zone - cfg->zones),
                            cfg->reverse_direction ? 1.0 - position : position, zone->length, color);
    case EFFECT_LIGHTNING:
        return scale(color, st->flash);
    case EFFECT_TUNE:
        if (zone->length == 1)
            return scale(color, st->audio_level);
        return position < st->audio_level ? color : (Rgb){0, 0, 0};
    case EFFECT_SLEEP:
        return scale(color, 0.05 + 0.35 * (1.0 - cos(2.0 * G_PI * t * 0.04 * speed)) / 2.0);
    case EFFECT_CPU: {
        double span = (double)cfg->cpu_max - cfg->cpu_min;
        double level = CLAMP((st->cpu_temp - cfg->cpu_min) / span, 0.0, 1.0);
        return hue_color((1.0 - level) * (2.0 / 3.0));
    }
    default:
        return color;
    }
}

static void render(const Config *cfg, const EffectState *st, double t, Rgb frame[SLOT_COUNT])
{
    double brightness = cfg->brightness / 100.0;

    memset(frame, 0, sizeof(Rgb) * SLOT_COUNT);
    if (!cfg->enabled)
        return;

    for (int z = 0; z < ZONE_COUNT; z++) {
        const ZoneConfig *zone = &cfg->zones[z];

        if (!zone->enabled)
            continue;
        for (int i = 0; i < zone->length; i++) {
            int physical = zone->reverse ? zone->length - 1 - i : i;
            int slot = zone_slot((Zone)z, physical);

            frame[slot] = scale(render_led(cfg, st, zone, slot, i, t), brightness);
        }
    }
}

/* Best effort: the board keeps working if the GPU helper is not installed. */
static void gpu_update(GpuLink *gpu, Rgb color, double now)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = GPU_SOCKET};
    const uint8_t message[3] = {color.r, color.g, color.b};
    double elapsed = now - gpu->last_sent;

    if (elapsed < GPU_SEND_INTERVAL)
        return;
    if (gpu->sent && memcmp(&color, &gpu->last, sizeof color) == 0 && elapsed < GPU_RESEND_INTERVAL)
        return;
    if (gpu->fd < 0 && (gpu->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
        return;

    gpu->last_sent = now;
    if (sendto(gpu->fd, message, sizeof message, 0, (struct sockaddr *)&address, sizeof address) == sizeof message) {
        gpu->last = color;
        gpu->sent = TRUE;
    }
}

static gboolean config_changed(struct timespec *last)
{
    struct stat info;
    char *path = config_path();
    int result = stat(path, &info);

    g_free(path);
    if (result != 0 || (info.st_mtim.tv_sec == last->tv_sec && info.st_mtim.tv_nsec == last->tv_nsec))
        return FALSE;
    *last = info.st_mtim;
    return TRUE;
}

int main(void)
{
    Config cfg;
    EffectState state = {0};
    Rgb frame[SLOT_COUNT], sent[SLOT_COUNT];
    struct timespec config_time = {0};
    GThread *audio = NULL;
    GpuLink gpu = {.fd = -1};
    int fd = -1;
    double previous = monotonic_seconds();
    double next_reload = 0, next_connect = 0, last_sent = 0;
    gboolean have_sent = FALSE;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    config_load(&cfg);

    while (!stop_requested) {
        double now = monotonic_seconds();
        double dt = MIN(now - previous, 0.5);

        previous = now;
        if (now >= next_reload) {
            next_reload = now + RELOAD_INTERVAL;
            if (config_changed(&config_time))
                config_load(&cfg);
        }
        audio = audio_update(audio, cfg.enabled && cfg.effect == EFFECT_TUNE);

        update_state(&cfg, &state, now, dt);
        render(&cfg, &state, now, frame);
        gpu_update(&gpu, frame[GPU_SLOT], now);

        if (fd < 0 && now >= next_connect) {
            char path[PATH_MAX];

            next_connect = now + RECONNECT_INTERVAL;
            if (device_find(path, sizeof path))
                fd = open(path, O_WRONLY | O_CLOEXEC);
            have_sent = FALSE;
        }

        if (fd >= 0 && (!have_sent || now - last_sent >= RESEND_INTERVAL || memcmp(frame, sent, sizeof frame) != 0)) {
            if (device_send(fd, frame, cfg.order)) {
                memcpy(sent, frame, sizeof frame);
                last_sent = now;
                have_sent = TRUE;
            } else {
                close(fd);
                fd = -1;
                next_connect = now + RECONNECT_INTERVAL;
            }
        }

        double remaining = FRAME_SECONDS - (monotonic_seconds() - now);
        if (remaining > 0)
            g_usleep((gulong)(remaining * 1e6));
    }

    audio_update(audio, FALSE);
    if (fd >= 0)
        close(fd);
    return 0;
}
