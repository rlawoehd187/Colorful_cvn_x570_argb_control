/* Settings window for colorful-rgbd. Saves the configuration; the daemon picks
 * up changes and drives the LEDs. */
#include "rgb-core.h"

#include <adwaita.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#define SERVICE "colorful-rgbd.service"
#define SAVE_DELAY_MS 80
#define STATUS_INTERVAL_S 2

static const struct {
    const char *name;
    const char *hex;
} PRESETS[] = {
    {"빨강", "#ff0000"}, {"초록", "#00ff00"}, {"파랑", "#0000ff"}, {"청록", "#00ffff"},
    {"자홍", "#ff00ff"}, {"노랑", "#ffff00"}, {"흰색", "#ffffff"},
};

typedef struct {
    GtkWidget *enabled;
    GtkWidget *length;
    GtkWidget *reverse;
    GtkWidget *use_color;
    GtkWidget *color;
} ZoneRow;

typedef struct {
    GtkWidget *enabled;
    GtkWidget *effect;
    GtkWidget *color_label;
    GtkWidget *color;
    GtkWidget *presets;
    GtkWidget *preset_buttons[G_N_ELEMENTS(PRESETS)];
    GtkWidget *color_info;
    GtkWidget *color_preview;
    GtkWidget *color_value;
    GtkWidget *speed_label;
    GtkWidget *speed;
    GtkWidget *reverse_direction;
    GtkWidget *brightness;
    GtkWidget *sensitivity_label;
    GtkWidget *sensitivity;
    GtkWidget *cpu_label;
    GtkWidget *cpu_box;
    GtkWidget *cpu_min;
    GtkWidget *cpu_max;
    GtkWidget *order;
    GtkWidget *autostart;
    GtkWidget *status;
    ZoneRow zones[ZONE_COUNT];
} Widgets;

static Widgets ui;
static Config cfg;
static guint save_source;
static gboolean syncing;

static GdkRGBA to_gdk(Rgb color)
{
    return (GdkRGBA){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f};
}

static Rgb from_gdk(const GdkRGBA *color)
{
    return (Rgb){(uint8_t)(color->red * 255 + 0.5f), (uint8_t)(color->green * 255 + 0.5f),
                 (uint8_t)(color->blue * 255 + 0.5f)};
}

static void set_button_color(GtkWidget *button, Rgb color)
{
    GdkRGBA rgba = to_gdk(color);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
}

static Rgb button_color(GtkWidget *button)
{
    return from_gdk(gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button)));
}

static gboolean run_systemctl(const char *arguments, char **output)
{
    char *command = g_strdup_printf("systemctl --user %s " SERVICE, arguments);
    int status = 1;
    gboolean spawned = g_spawn_command_line_sync(command, output, NULL, &status, NULL);

    g_free(command);
    return spawned && g_spawn_check_wait_status(status, NULL);
}

static gboolean update_status(gpointer unused)
{
    char path[PATH_MAX];
    char *message;
    gboolean problem = TRUE;

    (void)unused;
    if (!device_find(path, sizeof path)) {
        message = g_strdup("RGB 컨트롤러(2f4c:1000)를 찾을 수 없습니다.");
    } else if (access(path, W_OK) != 0) {
        message = g_strdup_printf("%s 쓰기 권한이 없습니다. udev 규칙을 확인해 주세요.", path);
    } else if (!run_systemctl("is-active --quiet", NULL)) {
        message = g_strdup("상주 서비스가 꺼져 있어 변경 사항이 LED에 적용되지 않습니다.");
    } else {
        message = g_strdup_printf("동작 중 (%s)", path);
        problem = FALSE;
    }

    gtk_label_set_text(GTK_LABEL(ui.status), message);
    if (problem)
        gtk_widget_add_css_class(ui.status, "error");
    else
        gtk_widget_remove_css_class(ui.status, "error");
    g_free(message);
    return G_SOURCE_CONTINUE;
}

static gboolean save_timeout(gpointer unused)
{
    GError *error = NULL;

    (void)unused;
    save_source = 0;
    if (!config_save(&cfg, &error)) {
        char *message = g_strdup_printf("설정 저장 실패: %s", error->message);

        gtk_label_set_text(GTK_LABEL(ui.status), message);
        gtk_widget_add_css_class(ui.status, "error");
        g_free(message);
        g_error_free(error);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_save(void)
{
    if (!save_source)
        save_source = g_timeout_add(SAVE_DELAY_MS, save_timeout, NULL);
}

static char *format_color(Rgb color)
{
    return g_strdup_printf("#%02X%02X%02X   R %d   G %d   B %d", color.r, color.g, color.b, color.r, color.g, color.b);
}

static void show_color_tooltip(GtkWidget *button, Rgb color)
{
    char *text = format_color(color);

    gtk_widget_set_tooltip_text(button, text);
    g_free(text);
}

static void draw_color_preview(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer unused)
{
    Rgb color = cfg.colors[cfg.effect];

    (void)area;
    (void)unused;
    cairo_set_source_rgb(cr, color.r / 255.0, color.g / 255.0, color.b / 255.0);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
}

static void update_color_info(void)
{
    Rgb color = cfg.colors[cfg.effect];
    char *text = format_color(color);

    gtk_label_set_text(GTK_LABEL(ui.color_value), text);
    g_free(text);
    gtk_widget_queue_draw(ui.color_preview);

    for (guint i = 0; i < G_N_ELEMENTS(PRESETS); i++) {
        GdkRGBA parsed;
        Rgb preset;

        gdk_rgba_parse(&parsed, PRESETS[i].hex);
        preset = from_gdk(&parsed);
        if (preset.r == color.r && preset.g == color.g && preset.b == color.b)
            gtk_widget_add_css_class(ui.preset_buttons[i], "selected-preset");
        else
            gtk_widget_remove_css_class(ui.preset_buttons[i], "selected-preset");
    }
}

static void update_visibility(void)
{
    gboolean color = effect_uses_color(cfg.effect);
    gboolean speed = effect_uses_speed(cfg.effect);
    gboolean tune = cfg.effect == EFFECT_TUNE;
    gboolean cpu = cfg.effect == EFFECT_CPU;

    gtk_widget_set_visible(ui.color_label, color);
    gtk_widget_set_visible(ui.color, color);
    gtk_widget_set_visible(ui.presets, color);
    gtk_widget_set_visible(ui.color_info, color);
    gtk_widget_set_visible(ui.speed_label, speed);
    gtk_widget_set_visible(ui.speed, speed);
    gtk_widget_set_visible(ui.reverse_direction, effect_uses_direction(cfg.effect));
    gtk_widget_set_visible(ui.sensitivity_label, tune);
    gtk_widget_set_visible(ui.sensitivity, tune);
    gtk_widget_set_visible(ui.cpu_label, cpu);
    gtk_widget_set_visible(ui.cpu_box, cpu);
}

/* Copies every control except the effect colour, which has its own handler. */
static void read_controls(void)
{
    cfg.enabled = gtk_switch_get_active(GTK_SWITCH(ui.enabled));
    cfg.speed = (int)gtk_range_get_value(GTK_RANGE(ui.speed));
    cfg.reverse_direction = gtk_check_button_get_active(GTK_CHECK_BUTTON(ui.reverse_direction));
    cfg.brightness = (int)gtk_range_get_value(GTK_RANGE(ui.brightness));
    cfg.sensitivity = (int)gtk_range_get_value(GTK_RANGE(ui.sensitivity));
    cfg.cpu_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.cpu_min));
    cfg.cpu_max = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.cpu_max));
    if (cfg.cpu_max <= cfg.cpu_min)
        cfg.cpu_max = cfg.cpu_min + 1;
    cfg.order = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(ui.order));

    for (int i = 0; i < ZONE_COUNT; i++) {
        const ZoneRow *row = &ui.zones[i];
        ZoneConfig *zone = &cfg.zones[i];

        zone->enabled = gtk_switch_get_active(GTK_SWITCH(row->enabled));
        zone->length = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(row->length));
        zone->reverse = gtk_check_button_get_active(GTK_CHECK_BUTTON(row->reverse));
        zone->use_color = gtk_check_button_get_active(GTK_CHECK_BUTTON(row->use_color));
        zone->color = button_color(row->color);
        show_color_tooltip(row->color, zone->color);
    }
}

static void controls_changed(void)
{
    if (syncing)
        return;
    read_controls();
    schedule_save();
}

static void on_notify(GObject *object, GParamSpec *pspec, gpointer unused)
{
    (void)object;
    (void)pspec;
    (void)unused;
    controls_changed();
}

static void on_value(GtkWidget *widget, gpointer unused)
{
    (void)widget;
    (void)unused;
    controls_changed();
}

static void on_color(GObject *object, GParamSpec *pspec, gpointer unused)
{
    (void)object;
    (void)pspec;
    (void)unused;
    if (syncing)
        return;
    cfg.colors[cfg.effect] = button_color(ui.color);
    update_color_info();
    schedule_save();
}

static void on_effect(GObject *object, GParamSpec *pspec, gpointer unused)
{
    (void)object;
    (void)pspec;
    (void)unused;
    if (syncing)
        return;
    cfg.effect = (Effect)gtk_drop_down_get_selected(GTK_DROP_DOWN(ui.effect));
    syncing = TRUE;
    set_button_color(ui.color, cfg.colors[cfg.effect]);
    syncing = FALSE;
    update_visibility();
    update_color_info();
    schedule_save();
}

static void on_preset(GtkButton *button, gpointer index)
{
    GdkRGBA rgba;

    (void)button;
    gdk_rgba_parse(&rgba, PRESETS[GPOINTER_TO_INT(index)].hex);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(ui.color), &rgba);
}

static void on_autostart(GObject *object, GParamSpec *pspec, gpointer unused)
{
    (void)object;
    (void)pspec;
    (void)unused;
    if (syncing)
        return;
    run_systemctl(gtk_switch_get_active(GTK_SWITCH(ui.autostart)) ? "enable --now" : "disable --now", NULL);
    update_status(NULL);
}

static GtkWidget *label(const char *text)
{
    GtkWidget *widget = gtk_label_new(text);

    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    return widget;
}

static GtkWidget *new_color_button(void)
{
    GtkColorDialog *dialog = gtk_color_dialog_new();

    gtk_color_dialog_set_with_alpha(dialog, FALSE);
    return gtk_color_dialog_button_new(dialog);
}

static GtkWidget *new_scale(int min, int max)
{
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);

    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_widget_set_hexpand(scale, TRUE);
    return scale;
}

static void load_css(void)
{
    GString *css = g_string_new(NULL);

    for (guint i = 0; i < G_N_ELEMENTS(PRESETS); i++)
        g_string_append_printf(css,
                               ".swatch-%u { background: %s; min-width: 32px; min-height: 32px;"
                               " padding: 0; border-radius: 16px; }\n",
                               i, PRESETS[i].hex);

    g_string_append(css, ".selected-preset { outline: 3px solid @accent_color; outline-offset: 2px; }\n");

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css->str);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_string_free(css, TRUE);
}

static GtkWidget *build_presets(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    for (guint i = 0; i < G_N_ELEMENTS(PRESETS); i++) {
        char css_class[16];
        GtkWidget *button = gtk_button_new();

        snprintf(css_class, sizeof css_class, "swatch-%u", i);
        gtk_widget_add_css_class(button, css_class);
        gtk_widget_set_tooltip_text(button, PRESETS[i].name);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, PRESETS[i].name, -1);
        ui.preset_buttons[i] = button;
        g_signal_connect(button, "clicked", G_CALLBACK(on_preset), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(box), button);
    }
    return box;
}

static GtkWidget *build_zones(void)
{
    GtkWidget *grid = gtk_grid_new();
    static const char *const HEADINGS[] = {"구역", "사용", "LED 수", "역방향", "개별 색상"};

    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_widget_set_margin_top(grid, 10);

    for (guint i = 0; i < G_N_ELEMENTS(HEADINGS); i++) {
        GtkWidget *heading = label(HEADINGS[i]);

        gtk_widget_add_css_class(heading, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), heading, (int)i, 0, 1, 1);
    }

    for (int i = 0; i < ZONE_COUNT; i++) {
        ZoneRow *row = &ui.zones[i];
        const ZoneConfig *zone = &cfg.zones[i];
        GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        row->enabled = gtk_switch_new();
        gtk_widget_set_valign(row->enabled, GTK_ALIGN_CENTER);
        gtk_switch_set_active(GTK_SWITCH(row->enabled), zone->enabled);

        row->length = gtk_spin_button_new_with_range(1, ZONES[i].max_leds, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->length), zone->length);
        gtk_widget_set_sensitive(row->length, i >= ZONE_5V_1 && i <= ZONE_5V_3);

        row->reverse = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(row->reverse), zone->reverse);

        row->use_color = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(row->use_color), zone->use_color);
        row->color = new_color_button();
        set_button_color(row->color, zone->color);
        show_color_tooltip(row->color, zone->color);
        gtk_box_append(GTK_BOX(color_box), row->use_color);
        gtk_box_append(GTK_BOX(color_box), row->color);

        gtk_grid_attach(GTK_GRID(grid), label(ZONES[i].label), 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->enabled, 1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->length, 2, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->reverse, 3, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), color_box, 4, i + 1, 1, 1);

        g_signal_connect(row->enabled, "notify::active", G_CALLBACK(on_notify), NULL);
        g_signal_connect(row->length, "value-changed", G_CALLBACK(on_value), NULL);
        g_signal_connect(row->reverse, "toggled", G_CALLBACK(on_value), NULL);
        g_signal_connect(row->use_color, "toggled", G_CALLBACK(on_value), NULL);
        g_signal_connect(row->color, "notify::rgba", G_CALLBACK(on_notify), NULL);
    }

    GtkWidget *expander = gtk_expander_new("구역 설정");
    gtk_expander_set_child(GTK_EXPANDER(expander), grid);
    return expander;
}

static void build_controls(void)
{
    const char *effects[EFFECT_COUNT + 1] = {NULL};
    const char *orders[ORDER_COUNT + 1] = {NULL};

    for (int i = 0; i < EFFECT_COUNT; i++)
        effects[i] = EFFECT_LABELS[i];
    for (int i = 0; i < ORDER_COUNT; i++)
        orders[i] = ORDER_NAMES[i];

    ui.enabled = gtk_switch_new();
    gtk_widget_set_halign(ui.enabled, GTK_ALIGN_END);
    gtk_switch_set_active(GTK_SWITCH(ui.enabled), cfg.enabled);

    ui.effect = gtk_drop_down_new_from_strings(effects);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ui.effect), cfg.effect);

    ui.color_label = label("색상");
    ui.color = new_color_button();
    set_button_color(ui.color, cfg.colors[cfg.effect]);
    ui.presets = build_presets();

    ui.color_preview = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ui.color_preview), 96);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.color_preview), 36);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.color_preview), draw_color_preview, NULL, NULL);
    GtkWidget *preview_frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(preview_frame), ui.color_preview);

    ui.color_value = gtk_label_new(NULL);
    gtk_label_set_selectable(GTK_LABEL(ui.color_value), TRUE);
    gtk_widget_add_css_class(ui.color_value, "monospace");

    ui.color_info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(ui.color_info, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(ui.color_info), preview_frame);
    gtk_box_append(GTK_BOX(ui.color_info), ui.color_value);

    ui.speed_label = label("속도");
    ui.speed = new_scale(1, 5);
    gtk_range_set_value(GTK_RANGE(ui.speed), cfg.speed);

    ui.reverse_direction = gtk_check_button_new_with_label("반대 방향으로 흐르기");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ui.reverse_direction), cfg.reverse_direction);

    ui.brightness = new_scale(0, 100);
    gtk_range_set_value(GTK_RANGE(ui.brightness), cfg.brightness);

    ui.sensitivity_label = label("감도");
    ui.sensitivity = new_scale(1, 20);
    gtk_range_set_value(GTK_RANGE(ui.sensitivity), cfg.sensitivity);

    ui.cpu_label = label("온도 범위 (°C)");
    ui.cpu_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.cpu_min = gtk_spin_button_new_with_range(20, 110, 1);
    ui.cpu_max = gtk_spin_button_new_with_range(21, 120, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.cpu_min), cfg.cpu_min);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.cpu_max), cfg.cpu_max);
    gtk_box_append(GTK_BOX(ui.cpu_box), ui.cpu_min);
    gtk_box_append(GTK_BOX(ui.cpu_box), gtk_label_new("~"));
    gtk_box_append(GTK_BOX(ui.cpu_box), ui.cpu_max);

    ui.order = gtk_drop_down_new_from_strings(orders);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ui.order), (guint)cfg.order);

    ui.autostart = gtk_switch_new();
    gtk_widget_set_halign(ui.autostart, GTK_ALIGN_END);
    gtk_switch_set_active(GTK_SWITCH(ui.autostart), run_systemctl("is-enabled --quiet", NULL));

    ui.status = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(ui.status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui.status), 0);
    gtk_widget_add_css_class(ui.status, "dim-label");
}

static void connect_controls(void)
{
    g_signal_connect(ui.enabled, "notify::active", G_CALLBACK(on_notify), NULL);
    g_signal_connect(ui.effect, "notify::selected", G_CALLBACK(on_effect), NULL);
    g_signal_connect(ui.color, "notify::rgba", G_CALLBACK(on_color), NULL);
    g_signal_connect(ui.speed, "value-changed", G_CALLBACK(on_value), NULL);
    g_signal_connect(ui.reverse_direction, "toggled", G_CALLBACK(on_value), NULL);
    g_signal_connect(ui.brightness, "value-changed", G_CALLBACK(on_value), NULL);
    g_signal_connect(ui.sensitivity, "value-changed", G_CALLBACK(on_value), NULL);
    g_signal_connect(ui.cpu_min, "value-changed", G_CALLBACK(on_value), NULL);
    g_signal_connect(ui.cpu_max, "value-changed", G_CALLBACK(on_value), NULL);
    g_signal_connect(ui.order, "notify::selected", G_CALLBACK(on_notify), NULL);
    g_signal_connect(ui.autostart, "notify::active", G_CALLBACK(on_autostart), NULL);
}

static void on_activate(GtkApplication *application, gpointer unused)
{
    (void)unused;
    config_load(&cfg);
    load_css();

    /* Signals are connected after the controls hold their saved values, so
     * opening the window neither rewrites the settings nor changes the LEDs. */
    syncing = TRUE;
    build_controls();
    GtkWidget *zones = build_zones();
    connect_controls();
    syncing = FALSE;

    GtkWidget *grid = gtk_grid_new();
    int row = 0;

    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_widget_set_margin_start(grid, 20);
    gtk_widget_set_margin_end(grid, 20);
    gtk_widget_set_margin_top(grid, 20);
    gtk_widget_set_margin_bottom(grid, 20);

#define ROW(left, right)                                     \
    do {                                                     \
        gtk_grid_attach(GTK_GRID(grid), (left), 0, row, 1, 1);  \
        gtk_grid_attach(GTK_GRID(grid), (right), 1, row, 1, 1); \
        row++;                                               \
    } while (0)
#define WIDE(widget)                                         \
    do {                                                     \
        gtk_grid_attach(GTK_GRID(grid), (widget), 0, row, 2, 1); \
        row++;                                               \
    } while (0)

    ROW(label("LED 켜기"), ui.enabled);
    ROW(label("효과"), ui.effect);
    ROW(ui.color_label, ui.color);
    WIDE(ui.color_info);
    WIDE(ui.presets);
    ROW(ui.speed_label, ui.speed);
    WIDE(ui.reverse_direction);
    ROW(label("밝기"), ui.brightness);
    ROW(ui.sensitivity_label, ui.sensitivity);
    ROW(ui.cpu_label, ui.cpu_box);
    ROW(label("색상 순서"), ui.order);
    WIDE(zones);
    ROW(label("로그인 시 자동 실행"), ui.autostart);
    WIDE(ui.status);

#undef ROW
#undef WIDE

    update_visibility();
    update_color_info();
    update_status(NULL);
    g_timeout_add_seconds(STATUS_INTERVAL_S, update_status, NULL);

    GtkWidget *window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window), "Colorful RGB");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, -1);
    gtk_window_set_child(GTK_WINDOW(window), grid);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    AdwApplication *application = adw_application_new("local.colorful.RgbControl", G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(application, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
