/*
 * Radar Display Renderer Implementation
 */

#include "radar_renderer.h"
#include "aircraft_store.h"
#include "radar_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "radar_renderer";

// Configuration (set at runtime)
static char s_display_label[32] = "RADAR - 50NM";  // Default label
static float s_sweep_degrees_per_frame = SWEEP_DEGREES_PER_FRAME;  // Default from radar_config.h
static bool s_show_aircraft_labels = true;  // Default: show labels

// UI elements
static lv_obj_t *s_radar_container = NULL;
static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_status_label = NULL;  // Aircraft count + last update
static lv_obj_t *s_ring_10nm = NULL;
static lv_obj_t *s_ring_25nm = NULL;
static lv_obj_t *s_ring_50nm = NULL;
static lv_obj_t *s_cardinal_n = NULL;
static lv_obj_t *s_cardinal_e = NULL;
static lv_obj_t *s_cardinal_s = NULL;
static lv_obj_t *s_cardinal_w = NULL;
static lv_obj_t *s_sweep_line = NULL;
static lv_obj_t *s_sweep_trail = NULL;

// Config button
static lv_obj_t *s_config_button = NULL;
static lv_obj_t *s_config_label = NULL;
typedef void (*config_button_callback_t)(void);
static config_button_callback_t s_config_callback = NULL;

// Sweep animation state
static lv_timer_t *s_sweep_timer = NULL;
static float s_sweep_angle = 0.0f;  // Current angle in degrees (0-360)

// Aircraft rendering
#define MAX_AIRCRAFT_BLIPS 64
#define VELOCITY_VECTOR_SCALE 0.2f  // Pixels per knot of speed
typedef struct {
    char hex[8];           // Aircraft ID for tracking
    lv_obj_t *blip;        // Circle blip
    lv_obj_t *label_cs;    // Callsign label
    lv_obj_t *label_alt;   // Altitude label
    lv_obj_t *velocity_line;  // Velocity vector line
    bool active;
} aircraft_blip_t;

static aircraft_blip_t s_blips[MAX_AIRCRAFT_BLIPS];
static int s_blip_count = 0;

// Velocity vector line points (persistent storage for LVGL)
static lv_point_precise_t s_velocity_points[MAX_AIRCRAFT_BLIPS][2];

// Forward declarations
static void create_distance_rings(lv_obj_t *parent);
static void create_cardinal_markers(lv_obj_t *parent);
static void create_sweep_elements(lv_obj_t *parent);
static void create_config_button(lv_obj_t *parent);
static void config_button_event_callback(lv_event_t *e);
static void sweep_timer_callback(lv_timer_t *timer);
static lv_color_t get_altitude_color(int altitude_ft);
static int find_blip(const char *hex);
static void delete_blip(int index);

bool radar_renderer_init(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Initializing radar renderer...");

    if (parent == NULL) {
        ESP_LOGE(TAG, "Parent object is NULL");
        return false;
    }

    // Create radar container (full screen)
    s_radar_container = lv_obj_create(parent);
    if (s_radar_container == NULL) {
        ESP_LOGE(TAG, "Failed to create radar container");
        return false;
    }

    // Configure container
    lv_obj_set_size(s_radar_container, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(s_radar_container, 0, 0);
    lv_obj_set_style_bg_color(s_radar_container,
                               lv_color_make(COLOR_BACKGROUND_R,
                                           COLOR_BACKGROUND_G,
                                           COLOR_BACKGROUND_B), 0);
    lv_obj_set_style_bg_opa(s_radar_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_radar_container, 0, 0);
    lv_obj_set_style_pad_all(s_radar_container, 0, 0);
    lv_obj_clear_flag(s_radar_container, LV_OBJ_FLAG_SCROLLABLE);

    // Create distance rings (10, 25, 50 nm)
    create_distance_rings(s_radar_container);

    // Create cardinal direction markers (N, E, S, W)
    create_cardinal_markers(s_radar_container);

    // Create sweep line and trail
    create_sweep_elements(s_radar_container);

    // Create title label at top
    s_title_label = lv_label_create(s_radar_container);
    lv_label_set_text(s_title_label, s_display_label);
    lv_obj_set_style_text_color(s_title_label,
                                 lv_color_make(COLOR_SWEEP_R,
                                             COLOR_SWEEP_G,
                                             COLOR_SWEEP_B), 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 20);

    // Create config button in top-right
    create_config_button(s_radar_container);

    // Create status label at bottom
    s_status_label = lv_label_create(s_radar_container);
    lv_label_set_text(s_status_label, "0 aircraft");
    lv_obj_set_style_text_color(s_status_label,
                                 lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Initialize aircraft blips array
    memset(s_blips, 0, sizeof(s_blips));
    s_blip_count = 0;

    ESP_LOGI(TAG, "Radar renderer initialized successfully");
    return true;
}

lv_obj_t *radar_renderer_get_container(void)
{
    return s_radar_container;
}

void radar_renderer_set_label(const char *label)
{
    if (label != NULL) {
        strncpy(s_display_label, label, sizeof(s_display_label) - 1);
        s_display_label[sizeof(s_display_label) - 1] = '\0';

        // Update label if already created
        if (s_title_label != NULL) {
            lv_label_set_text(s_title_label, s_display_label);
        }

        ESP_LOGI(TAG, "Display label set to: %s", s_display_label);
    }
}

void radar_renderer_set_sweep_rate(float sweep_seconds)
{
    // Calculate degrees per frame: 360° / (sweep_seconds * 60 FPS)
    float old_rate = s_sweep_degrees_per_frame;
    s_sweep_degrees_per_frame = 360.0f / (sweep_seconds * 60.0f);
    ESP_LOGI(TAG, "Sweep rate changed: %.2f°/frame -> %.2f°/frame (%.1fs per rotation)",
             old_rate, s_sweep_degrees_per_frame, sweep_seconds);
}

void radar_renderer_set_show_labels(bool show_labels)
{
    s_show_aircraft_labels = show_labels;
    ESP_LOGI(TAG, "Aircraft labels %s", show_labels ? "enabled" : "disabled");
}

void radar_renderer_set_config_callback(config_button_callback_t callback)
{
    s_config_callback = callback;
    ESP_LOGI(TAG, "Config button callback registered");
}

void radar_renderer_start_sweep(void)
{
    if (s_sweep_timer == NULL) {
        // Create timer for 60Hz sweep animation (16ms interval)
        s_sweep_timer = lv_timer_create(sweep_timer_callback, SWEEP_TIMER_MS, NULL);
        ESP_LOGI(TAG, "Sweep animation started (60 Hz, %.2f°/frame = %.1fs/rotation)",
                 s_sweep_degrees_per_frame, 360.0f / (s_sweep_degrees_per_frame * 60.0f));
    }
}

void radar_renderer_pause_sweep(void)
{
    if (s_sweep_timer != NULL) {
        lv_timer_pause(s_sweep_timer);
        ESP_LOGI(TAG, "Sweep animation paused");
    }
}

void radar_renderer_resume_sweep(void)
{
    if (s_sweep_timer != NULL) {
        lv_timer_resume(s_sweep_timer);
        ESP_LOGI(TAG, "Sweep animation resumed");
    } else {
        // Timer doesn't exist, start it
        radar_renderer_start_sweep();
    }
}

void radar_renderer_stop_sweep(void)
{
    if (s_sweep_timer != NULL) {
        lv_timer_del(s_sweep_timer);
        s_sweep_timer = NULL;
        ESP_LOGI(TAG, "Sweep animation stopped");
    }
}

void radar_renderer_debug_overlay(bool enable)
{
    // TODO: Add debug overlay showing center point and exact ring positions
    ESP_LOGI(TAG, "Debug overlay %s (not yet implemented)", enable ? "enabled" : "disabled");
}

// Internal functions

static void create_distance_rings(lv_obj_t *parent)
{
    // 10nm ring (innermost)
    s_ring_10nm = lv_arc_create(parent);
    lv_obj_set_size(s_ring_10nm, RING_10NM_RADIUS * 2, RING_10NM_RADIUS * 2);
    lv_obj_center(s_ring_10nm);
    lv_arc_set_rotation(s_ring_10nm, 0);
    lv_arc_set_bg_angles(s_ring_10nm, 0, 360);
    lv_arc_set_value(s_ring_10nm, 0);
    lv_obj_remove_style(s_ring_10nm, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_ring_10nm, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring_10nm,
                                lv_color_make(COLOR_RING_R, COLOR_RING_G, COLOR_RING_B),
                                LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring_10nm, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_ring_10nm, LV_OPA_50, LV_PART_MAIN);
    lv_obj_clear_flag(s_ring_10nm, LV_OBJ_FLAG_CLICKABLE);

    // 25nm ring (middle)
    s_ring_25nm = lv_arc_create(parent);
    lv_obj_set_size(s_ring_25nm, RING_25NM_RADIUS * 2, RING_25NM_RADIUS * 2);
    lv_obj_center(s_ring_25nm);
    lv_arc_set_rotation(s_ring_25nm, 0);
    lv_arc_set_bg_angles(s_ring_25nm, 0, 360);
    lv_arc_set_value(s_ring_25nm, 0);
    lv_obj_remove_style(s_ring_25nm, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_ring_25nm, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring_25nm,
                                lv_color_make(COLOR_RING_R, COLOR_RING_G, COLOR_RING_B),
                                LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring_25nm, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_ring_25nm, LV_OPA_50, LV_PART_MAIN);
    lv_obj_clear_flag(s_ring_25nm, LV_OBJ_FLAG_CLICKABLE);

    // 50nm ring (outer)
    s_ring_50nm = lv_arc_create(parent);
    lv_obj_set_size(s_ring_50nm, RING_50NM_RADIUS * 2, RING_50NM_RADIUS * 2);
    lv_obj_center(s_ring_50nm);
    lv_arc_set_rotation(s_ring_50nm, 0);
    lv_arc_set_bg_angles(s_ring_50nm, 0, 360);
    lv_arc_set_value(s_ring_50nm, 0);
    lv_obj_remove_style(s_ring_50nm, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_ring_50nm, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring_50nm,
                                lv_color_make(COLOR_RING_R, COLOR_RING_G, COLOR_RING_B),
                                LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring_50nm, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_ring_50nm, LV_OPA_60, LV_PART_MAIN);
    lv_obj_clear_flag(s_ring_50nm, LV_OBJ_FLAG_CLICKABLE);

    ESP_LOGI(TAG, "Distance rings created: 10nm, 25nm, 50nm");
}

static void create_cardinal_markers(lv_obj_t *parent)
{
    // Cardinal direction positions just inside outer ring
    const int label_radius = RING_50NM_RADIUS - 30;  // Just inside outer ring

    // North (top, 90 degrees = -90 in our coordinate system)
    float angle_n = -90.0f * M_PI / 180.0f;
    int x_n = SCREEN_CENTER_X + (int)(label_radius * cosf(angle_n));
    int y_n = SCREEN_CENTER_Y + (int)(label_radius * sinf(angle_n));

    s_cardinal_n = lv_label_create(parent);
    lv_label_set_text(s_cardinal_n, "N");
    lv_obj_set_style_text_color(s_cardinal_n, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(s_cardinal_n, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_cardinal_n, x_n - 8, y_n - 8);

    // East (right, 0 degrees)
    float angle_e = 0.0f * M_PI / 180.0f;
    int x_e = SCREEN_CENTER_X + (int)(label_radius * cosf(angle_e));
    int y_e = SCREEN_CENTER_Y + (int)(label_radius * sinf(angle_e));

    s_cardinal_e = lv_label_create(parent);
    lv_label_set_text(s_cardinal_e, "E");
    lv_obj_set_style_text_color(s_cardinal_e, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(s_cardinal_e, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_cardinal_e, x_e - 8, y_e - 8);

    // South (bottom, 90 degrees)
    float angle_s = 90.0f * M_PI / 180.0f;
    int x_s = SCREEN_CENTER_X + (int)(label_radius * cosf(angle_s));
    int y_s = SCREEN_CENTER_Y + (int)(label_radius * sinf(angle_s));

    s_cardinal_s = lv_label_create(parent);
    lv_label_set_text(s_cardinal_s, "S");
    lv_obj_set_style_text_color(s_cardinal_s, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(s_cardinal_s, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_cardinal_s, x_s - 8, y_s - 8);

    // West (left, 180 degrees)
    float angle_w = 180.0f * M_PI / 180.0f;
    int x_w = SCREEN_CENTER_X + (int)(label_radius * cosf(angle_w));
    int y_w = SCREEN_CENTER_Y + (int)(label_radius * sinf(angle_w));

    s_cardinal_w = lv_label_create(parent);
    lv_label_set_text(s_cardinal_w, "W");
    lv_obj_set_style_text_color(s_cardinal_w, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(s_cardinal_w, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_cardinal_w, x_w - 8, y_w - 8);

    ESP_LOGI(TAG, "Cardinal markers created: N, E, S, W");
}

static void create_sweep_elements(lv_obj_t *parent)
{
    // Create sweep line from center to outer edge
    static lv_point_precise_t line_points[2];
    line_points[0].x = SCREEN_CENTER_X;  // Center point
    line_points[0].y = SCREEN_CENTER_Y;
    line_points[1].x = SCREEN_CENTER_X;  // Will be updated by rotation
    line_points[1].y = SCREEN_CENTER_Y - RADAR_DISPLAY_RADIUS;  // Point upward (North)

    s_sweep_line = lv_line_create(parent);
    lv_line_set_points(s_sweep_line, line_points, 2);
    lv_obj_set_style_line_color(s_sweep_line,
                                 lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_style_line_width(s_sweep_line, 2, 0);
    lv_obj_set_style_line_rounded(s_sweep_line, true, 0);

    // Set rotation pivot to center point
    lv_obj_set_style_transform_pivot_x(s_sweep_line, SCREEN_CENTER_X, 0);
    lv_obj_set_style_transform_pivot_y(s_sweep_line, SCREEN_CENTER_Y, 0);

    // Create sweep trail arc (30° wide, follows behind sweep)
    s_sweep_trail = lv_arc_create(parent);
    lv_obj_set_size(s_sweep_trail, RADAR_DISPLAY_RADIUS * 2, RADAR_DISPLAY_RADIUS * 2);
    lv_obj_center(s_sweep_trail);
    lv_arc_set_rotation(s_sweep_trail, 270);  // Start at North (270 in LVGL coords)
    lv_arc_set_bg_angles(s_sweep_trail, 0, 0);  // Will be updated
    lv_arc_set_angles(s_sweep_trail, 0, SWEEP_TRAIL_DEGREES);  // 30° trail
    lv_obj_remove_style(s_sweep_trail, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_sweep_trail, NULL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sweep_trail,
                                lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B),
                                LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_sweep_trail, RADAR_DISPLAY_RADIUS, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_sweep_trail, LV_OPA_40, LV_PART_INDICATOR);  // Brighter trail
    lv_obj_clear_flag(s_sweep_trail, LV_OBJ_FLAG_CLICKABLE);

    ESP_LOGI(TAG, "Sweep elements created: line and trail arc");
}

static void create_config_button(lv_obj_t *parent)
{
    // Create button above South marker (centered at bottom)
    s_config_button = lv_btn_create(parent);
    lv_obj_set_size(s_config_button, 70, 40);

    // Position just above the South "S" marker
    // South marker is at ~(400, 760), button centered above it
    lv_obj_set_pos(s_config_button, SCREEN_CENTER_X - 35, 710);

    // Styling: blue background, sweep-color border
    lv_obj_set_style_bg_color(s_config_button, lv_color_make(0x40, 0x40, 0x80), 0);
    lv_obj_set_style_bg_opa(s_config_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_config_button,
                                  lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_style_border_width(s_config_button, 2, 0);
    lv_obj_set_style_radius(s_config_button, 8, 0);

    // Label
    s_config_label = lv_label_create(s_config_button);
    lv_label_set_text(s_config_label, "CONFIG");
    lv_obj_set_style_text_color(s_config_label,
                                lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_style_text_font(s_config_label, &lv_font_montserrat_12, 0);
    lv_obj_center(s_config_label);

    // Touch event
    lv_obj_add_event_cb(s_config_button, config_button_event_callback, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Config button created above South marker");
}

static void config_button_event_callback(lv_event_t *e)
{
    ESP_LOGI(TAG, "Config button pressed!");

    // Call registered callback if set
    if (s_config_callback != NULL) {
        s_config_callback();
    }
}

static void sweep_timer_callback(lv_timer_t *timer)
{
    // Debug: Log sweep rate every 10 rotations
    static int rotation_count = 0;
    static uint32_t last_rotation_time = 0;

    float prev_angle = s_sweep_angle;

    // Update sweep angle
    s_sweep_angle += s_sweep_degrees_per_frame;
    if (s_sweep_angle >= 360.0f) {
        s_sweep_angle -= 360.0f;
        rotation_count++;

        // Log timing every 10 rotations
        if (rotation_count % 10 == 0) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (last_rotation_time > 0) {
                uint32_t elapsed = now - last_rotation_time;
                float actual_seconds_per_rotation = elapsed / 10000.0f;
                ESP_LOGI(TAG, "Sweep: %.2f°/frame, actual %.2fs/rotation (expected %.1fs)",
                         s_sweep_degrees_per_frame, actual_seconds_per_rotation,
                         360.0f / (s_sweep_degrees_per_frame * 60.0f));
            }
            last_rotation_time = now;
        }
    }

    // Rotate sweep line
    // LVGL rotation is in 0.1 degree units, and 0° is right (East)
    // We want 0° to be North, so subtract 90°
    int rotation_decidegrees = (int)((s_sweep_angle - 90.0f) * 10.0f);
    lv_obj_set_style_transform_rotation(s_sweep_line, rotation_decidegrees, 0);

    // Update trail arc rotation to follow sweep
    // Trail arc rotation in LVGL: 270 = North, increases clockwise
    int arc_rotation = (int)(270.0f + s_sweep_angle);
    if (arc_rotation >= 360) {
        arc_rotation -= 360;
    }
    lv_arc_set_rotation(s_sweep_trail, arc_rotation);
}

void radar_renderer_update_aircraft(const void *aircraft_data, int count)
{
    const tracked_aircraft_t *aircraft = (const tracked_aircraft_t *)aircraft_data;

    if (s_radar_container == NULL) {
        ESP_LOGW(TAG, "Radar not initialized, skipping aircraft update");
        return;
    }

    ESP_LOGI(TAG, "Updating %d aircraft on radar display", count);

    // Lock LVGL before modifying objects
    bsp_display_lock(0);

    // Mark all blips as inactive for pruning
    for (int i = 0; i < MAX_AIRCRAFT_BLIPS; i++) {
        if (s_blips[i].active) {
            s_blips[i].active = false;  // Will be set true if found in new data
        }
    }

    // Use configured label visibility preference
    bool show_labels = s_show_aircraft_labels;

    // Update/create blips for each aircraft
    for (int i = 0; i < count; i++) {
        // Skip aircraft outside radar radius
        if (aircraft[i].distance_nm > RADAR_RADIUS_NM) {
            continue;
        }

        // Find existing blip or create new one
        int blip_idx = find_blip(aircraft[i].hex);
        if (blip_idx == -1) {
            // Find empty slot
            for (int j = 0; j < MAX_AIRCRAFT_BLIPS; j++) {
                if (s_blips[j].blip == NULL) {
                    blip_idx = j;
                    break;
                }
            }
        }

        if (blip_idx == -1) {
            ESP_LOGW(TAG, "No free blip slots for aircraft %s", aircraft[i].hex);
            continue;
        }

        // Create or update blip
        if (s_blips[blip_idx].blip == NULL) {
            // Create new blip (circle)
            s_blips[blip_idx].blip = lv_obj_create(s_radar_container);
            lv_obj_set_size(s_blips[blip_idx].blip, 8, 8);  // 8x8 pixel circle
            lv_obj_set_style_radius(s_blips[blip_idx].blip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(s_blips[blip_idx].blip, 0, 0);
            lv_obj_clear_flag(s_blips[blip_idx].blip, LV_OBJ_FLAG_SCROLLABLE);

            // Create callsign label
            s_blips[blip_idx].label_cs = lv_label_create(s_radar_container);
            lv_obj_set_style_text_font(s_blips[blip_idx].label_cs, &lv_font_montserrat_12, 0);

            // Create altitude label
            s_blips[blip_idx].label_alt = lv_label_create(s_radar_container);
            lv_obj_set_style_text_font(s_blips[blip_idx].label_alt, &lv_font_montserrat_12, 0);

            // Create velocity vector line
            s_blips[blip_idx].velocity_line = lv_line_create(s_radar_container);
            lv_obj_set_style_line_color(s_blips[blip_idx].velocity_line, lv_color_make(0x80, 0x80, 0x80), 0);  // Light grey
            lv_obj_set_style_line_width(s_blips[blip_idx].velocity_line, 1, 0);
            lv_obj_set_style_line_opa(s_blips[blip_idx].velocity_line, LV_OPA_70, 0);
            lv_obj_clear_flag(s_blips[blip_idx].velocity_line, LV_OBJ_FLAG_CLICKABLE);

            strncpy(s_blips[blip_idx].hex, aircraft[i].hex, sizeof(s_blips[blip_idx].hex) - 1);
        }

        // Update position
        lv_obj_set_pos(s_blips[blip_idx].blip, aircraft[i].screen_x - 4, aircraft[i].screen_y - 4);

        // Update color based on altitude
        lv_color_t color = get_altitude_color(aircraft[i].altitude);
        lv_obj_set_style_bg_color(s_blips[blip_idx].blip, color, 0);
        lv_obj_set_style_bg_opa(s_blips[blip_idx].blip, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_blips[blip_idx].label_cs, color, 0);
        lv_obj_set_style_text_color(s_blips[blip_idx].label_alt, color, 0);

        // Update labels
        if (show_labels && aircraft[i].callsign[0] != '\0') {
            lv_label_set_text(s_blips[blip_idx].label_cs, aircraft[i].callsign);
            lv_obj_set_pos(s_blips[blip_idx].label_cs, aircraft[i].screen_x + 6, aircraft[i].screen_y - 14);
            lv_obj_clear_flag(s_blips[blip_idx].label_cs, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_blips[blip_idx].label_cs, LV_OBJ_FLAG_HIDDEN);
        }

        // Format altitude (35000 → "350")
        if (show_labels && aircraft[i].altitude > 0) {
            char alt_str[8];
            snprintf(alt_str, sizeof(alt_str), "%d", aircraft[i].altitude / 100);
            lv_label_set_text(s_blips[blip_idx].label_alt, alt_str);
            lv_obj_set_pos(s_blips[blip_idx].label_alt, aircraft[i].screen_x + 6, aircraft[i].screen_y + 4);
            lv_obj_clear_flag(s_blips[blip_idx].label_alt, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_blips[blip_idx].label_alt, LV_OBJ_FLAG_HIDDEN);
        }

        // Update velocity vector
        if (aircraft[i].speed > 0 && aircraft[i].track >= 0) {
            // Calculate vector length based on speed (scale: 0.2 pixels per knot)
            float vector_length = aircraft[i].speed * VELOCITY_VECTOR_SCALE;

            // Convert track to radians (track: 0° = North, increases clockwise)
            // Adjust for screen coordinates: subtract 90° to rotate North to top
            float angle_rad = (aircraft[i].track - 90.0f) * M_PI / 180.0f;

            // Calculate endpoint
            int end_x = aircraft[i].screen_x + (int)(vector_length * cosf(angle_rad));
            int end_y = aircraft[i].screen_y + (int)(vector_length * sinf(angle_rad));

            // Update line points in persistent storage
            s_velocity_points[blip_idx][0].x = aircraft[i].screen_x;
            s_velocity_points[blip_idx][0].y = aircraft[i].screen_y;
            s_velocity_points[blip_idx][1].x = end_x;
            s_velocity_points[blip_idx][1].y = end_y;

            lv_line_set_points(s_blips[blip_idx].velocity_line, s_velocity_points[blip_idx], 2);
            lv_obj_clear_flag(s_blips[blip_idx].velocity_line, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Hide vector if no speed/track data
            lv_obj_add_flag(s_blips[blip_idx].velocity_line, LV_OBJ_FLAG_HIDDEN);
        }

        s_blips[blip_idx].active = true;
    }

    // Delete blips for aircraft that are no longer present
    for (int i = 0; i < MAX_AIRCRAFT_BLIPS; i++) {
        if (s_blips[i].blip != NULL && !s_blips[i].active) {
            delete_blip(i);
        }
    }

    // Count active blips
    s_blip_count = 0;
    for (int i = 0; i < MAX_AIRCRAFT_BLIPS; i++) {
        if (s_blips[i].blip != NULL) {
            s_blip_count++;
        }
    }

    // Update status label
    char status_str[32];
    snprintf(status_str, sizeof(status_str), "%d aircraft", s_blip_count);
    lv_label_set_text(s_status_label, status_str);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Radar display updated: %d blips rendered", s_blip_count);
}

// Helper functions

static lv_color_t get_altitude_color(int altitude_ft)
{
    // Color-code by altitude:
    // Yellow: < 10,000 ft (low)
    // Orange: 10,000 - 25,000 ft (medium)
    // White: > 25,000 ft (high)

    if (altitude_ft < 10000) {
        return lv_color_make(0xFF, 0xFF, 0x00);  // Yellow
    } else if (altitude_ft < 25000) {
        return lv_color_make(0xFF, 0x80, 0x00);  // Orange
    } else {
        return lv_color_make(0xFF, 0xFF, 0xFF);  // White
    }
}

static int find_blip(const char *hex)
{
    for (int i = 0; i < MAX_AIRCRAFT_BLIPS; i++) {
        if (s_blips[i].blip != NULL && strcmp(s_blips[i].hex, hex) == 0) {
            return i;
        }
    }
    return -1;
}

static void delete_blip(int index)
{
    if (s_blips[index].blip != NULL) {
        lv_obj_del(s_blips[index].blip);
        s_blips[index].blip = NULL;
    }
    if (s_blips[index].label_cs != NULL) {
        lv_obj_del(s_blips[index].label_cs);
        s_blips[index].label_cs = NULL;
    }
    if (s_blips[index].label_alt != NULL) {
        lv_obj_del(s_blips[index].label_alt);
        s_blips[index].label_alt = NULL;
    }
    if (s_blips[index].velocity_line != NULL) {
        lv_obj_del(s_blips[index].velocity_line);
        s_blips[index].velocity_line = NULL;
    }
    memset(s_blips[index].hex, 0, sizeof(s_blips[index].hex));
    s_blips[index].active = false;
}
