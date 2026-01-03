/*
 * Settings Panel - Touch-based Configuration Overlay Implementation
 * LVGL UI for radar configuration
 */

#include "settings_panel.h"
#include "radar_renderer.h"
#include "radar_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "settings_panel";

// Timezone dropdown options (newline-separated)
static const char *TIMEZONE_OPTIONS =
    "UTC-12\nUTC-11\nUTC-10\nUTC-9\nUTC-8\nUTC-7\nUTC-6\n"
    "UTC-5\nUTC-4\nUTC-3\nUTC-2\nUTC-1\nUTC\n"
    "UTC+1\nUTC+2\nUTC+3\nUTC+4\nUTC+5\nUTC+6\n"
    "UTC+7\nUTC+8\nUTC+9\nUTC+10\nUTC+11\nUTC+12\nUTC+13\nUTC+14";

// Helper functions for timezone offset conversion
static int timezone_offset_to_index(int8_t offset)
{
    return offset + 12;  // -12→0, 0→12, +14→26
}

static int8_t timezone_index_to_offset(int index)
{
    return index - 12;  // 0→-12, 12→0, 26→+14
}

// Static UI elements
static lv_obj_t *s_overlay = NULL;           // Semi-transparent background
static lv_obj_t *s_panel = NULL;             // Main settings panel
static lv_obj_t *s_keyboard = NULL;          // On-screen keyboard

// Input fields
static lv_obj_t *s_wifi_ssid_ta = NULL;      // WiFi SSID text area
static lv_obj_t *s_wifi_password_ta = NULL;  // WiFi password text area (masked)
static lv_obj_t *s_home_lat_ta = NULL;       // Home latitude text area
static lv_obj_t *s_home_lon_ta = NULL;       // Home longitude text area
static lv_obj_t *s_display_label_ta = NULL;  // Display label text area

// Sliders
static lv_obj_t *s_radius_slider = NULL;     // Radar radius slider
static lv_obj_t *s_radius_label = NULL;      // Radius value label

// Checkboxes
static lv_obj_t *s_show_labels_cb = NULL;    // Show aircraft labels checkbox

// Dropdowns
static lv_obj_t *s_timezone_dd = NULL;       // Timezone dropdown

// Buttons
static lv_obj_t *s_save_btn = NULL;
static lv_obj_t *s_cancel_btn = NULL;
static lv_obj_t *s_reset_btn = NULL;

// Save callback
static settings_save_callback_t s_save_callback = NULL;

// Configuration copy (for editing)
static radar_config_t s_current_config;

// Forward declarations
static void close_panel(void);
static void save_settings(void);
static void factory_reset(void);

/**
 * Validate latitude (-90 to +90)
 */
static bool validate_latitude(float lat)
{
    return (lat >= -90.0f && lat <= 90.0f);
}

/**
 * Validate longitude (-180 to +180)
 */
static bool validate_longitude(float lon)
{
    return (lon >= -180.0f && lon <= 180.0f);
}

/**
 * Text area focus event - show keyboard
 */
static void textarea_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (s_keyboard == NULL) {
        return;
    }

    // Set keyboard target and show
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    // For lat/lon fields, use number mode
    if (ta == s_home_lat_ta || ta == s_home_lon_ta) {
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
    } else {
        // Use TEXT_UPPER mode which has numbers available
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    }
}

/**
 * Text area defocus event - hide keyboard
 */
static void textarea_defocus_cb(lv_event_t *e)
{
    (void)e;
    if (s_keyboard != NULL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Keyboard ready event - user pressed OK/Enter
 */
static void keyboard_ready_cb(lv_event_t *e)
{
    (void)e;
    if (s_keyboard != NULL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Slider value changed event - update label
 */
static void radius_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);

    if (s_radius_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d NM", (int)value);
        lv_label_set_text(s_radius_label, buf);
    }
}

/**
 * SAVE button event
 */
static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    save_settings();
}

/**
 * CANCEL button event
 */
static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Settings canceled by user");
    close_panel();
}

/**
 * FACTORY RESET button event - show confirmation
 */
static void reset_btn_cb(lv_event_t *e)
{
    (void)e;

    // Create confirmation message box
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "FACTORY RESET");
    lv_msgbox_add_text(mbox, "Erase all settings?\nDevice will reboot.");
    lv_msgbox_add_close_button(mbox);

    lv_obj_t *btn_container = lv_msgbox_get_footer(mbox);
    lv_obj_t *yes_btn = lv_button_create(btn_container);
    lv_obj_t *yes_label = lv_label_create(yes_btn);
    lv_label_set_text(yes_label, "YES");
    lv_obj_center(yes_label);

    // Style YES button as red (destructive action)
    lv_obj_set_style_bg_color(yes_btn, lv_color_make(0xcc, 0x00, 0x00), 0);

    // Store event data for factory reset
    lv_obj_add_event_cb(yes_btn, (lv_event_cb_t)factory_reset, LV_EVENT_CLICKED, NULL);
}

/**
 * Save current settings from UI to config structure
 */
static void save_settings(void)
{
    ESP_LOGI(TAG, "Validating and saving settings...");

    // Read text inputs
    const char *ssid = lv_textarea_get_text(s_wifi_ssid_ta);
    const char *password = lv_textarea_get_text(s_wifi_password_ta);
    const char *lat_str = lv_textarea_get_text(s_home_lat_ta);
    const char *lon_str = lv_textarea_get_text(s_home_lon_ta);
    const char *label = lv_textarea_get_text(s_display_label_ta);

    // Read sliders
    int32_t radius = lv_slider_get_value(s_radius_slider);

    // Read checkboxes
    bool show_labels = (lv_obj_get_state(s_show_labels_cb) & LV_STATE_CHECKED) != 0;

    // Read dropdown
    int tz_index = lv_dropdown_get_selected(s_timezone_dd);
    int8_t timezone_offset = timezone_index_to_offset(tz_index);

    // Validate SSID (required if password is set)
    if (strlen(ssid) == 0 && strlen(password) > 0) {
        ESP_LOGW(TAG, "WiFi password set but SSID is empty");
        // Allow it - user might be clearing config
    }

    // Validate SSID length
    if (strlen(ssid) >= sizeof(s_current_config.wifi_ssid)) {
        ESP_LOGE(TAG, "SSID too long (max %d chars)", (int)sizeof(s_current_config.wifi_ssid) - 1);
        return;
    }

    // Validate password length
    if (strlen(password) >= sizeof(s_current_config.wifi_password)) {
        ESP_LOGE(TAG, "Password too long (max %d chars)", (int)sizeof(s_current_config.wifi_password) - 1);
        return;
    }

    // Parse and validate latitude
    float lat = 0.0f;
    if (sscanf(lat_str, "%f", &lat) != 1 || !validate_latitude(lat)) {
        ESP_LOGE(TAG, "Invalid latitude: %s (must be -90 to +90)", lat_str);
        return;
    }

    // Parse and validate longitude
    float lon = 0.0f;
    if (sscanf(lon_str, "%f", &lon) != 1 || !validate_longitude(lon)) {
        ESP_LOGE(TAG, "Invalid longitude: %s (must be -180 to +180)", lon_str);
        return;
    }

    // Validate label length
    if (strlen(label) >= sizeof(s_current_config.display_label)) {
        ESP_LOGE(TAG, "Label too long (max %d chars)", (int)sizeof(s_current_config.display_label) - 1);
        return;
    }

    // All validation passed - update config
    strncpy(s_current_config.wifi_ssid, ssid, sizeof(s_current_config.wifi_ssid) - 1);
    s_current_config.wifi_ssid[sizeof(s_current_config.wifi_ssid) - 1] = '\0';

    strncpy(s_current_config.wifi_password, password, sizeof(s_current_config.wifi_password) - 1);
    s_current_config.wifi_password[sizeof(s_current_config.wifi_password) - 1] = '\0';

    s_current_config.home_lat = lat;
    s_current_config.home_lon = lon;
    s_current_config.radar_radius_nm = (int)radius;
    s_current_config.show_aircraft_labels = show_labels;
    s_current_config.timezone_offset_hours = timezone_offset;

    strncpy(s_current_config.display_label, label, sizeof(s_current_config.display_label) - 1);
    s_current_config.display_label[sizeof(s_current_config.display_label) - 1] = '\0';

    ESP_LOGI(TAG, "Settings validated successfully");
    ESP_LOGI(TAG, "  WiFi: %s", s_current_config.wifi_ssid);
    ESP_LOGI(TAG, "  Home: %.4f, %.4f", s_current_config.home_lat, s_current_config.home_lon);
    ESP_LOGI(TAG, "  Radius: %d NM", s_current_config.radar_radius_nm);
    ESP_LOGI(TAG, "  Show Labels: %s", s_current_config.show_aircraft_labels ? "Yes" : "No");
    ESP_LOGI(TAG, "  Label: %s", s_current_config.display_label);

    // Call save callback if registered
    if (s_save_callback != NULL) {
        s_save_callback(&s_current_config);
    }

    // Close panel
    close_panel();
}

/**
 * Factory reset - erase all NVS and reboot
 */
static void factory_reset(void)
{
    ESP_LOGW(TAG, "FACTORY RESET requested - this will erase all settings");

    // Close any open message boxes
    close_panel();

    // Note: Actual NVS erase and reboot should be done by main.c via callback
    // For now, just log the request
    ESP_LOGW(TAG, "Factory reset implementation pending in main.c");
}

/**
 * Close settings panel and resume radar
 */
static void close_panel(void)
{
    ESP_LOGI(TAG, "Closing settings panel");

    // Delete all UI elements
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }

    s_panel = NULL;
    s_keyboard = NULL;
    s_wifi_ssid_ta = NULL;
    s_wifi_password_ta = NULL;
    s_home_lat_ta = NULL;
    s_home_lon_ta = NULL;
    s_display_label_ta = NULL;
    s_radius_slider = NULL;
    s_radius_label = NULL;
    s_show_labels_cb = NULL;
    s_timezone_dd = NULL;
    s_save_btn = NULL;
    s_cancel_btn = NULL;
    s_reset_btn = NULL;

    // Resume radar sweep
    radar_renderer_resume_sweep();
}

/**
 * Create settings panel UI
 */
void settings_panel_create(lv_obj_t *parent, const radar_config_t *current_cfg)
{
    if (s_overlay != NULL) {
        ESP_LOGW(TAG, "Settings panel already open");
        return;
    }

    if (current_cfg == NULL) {
        ESP_LOGE(TAG, "Cannot create settings panel: config is NULL");
        return;
    }

    ESP_LOGI(TAG, "Creating settings panel");

    // Copy current config
    memcpy(&s_current_config, current_cfg, sizeof(radar_config_t));

    // Pause radar sweep
    radar_renderer_pause_sweep();

    // Create semi-transparent overlay
    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Create main settings panel
    s_panel = lv_obj_create(s_overlay);
    lv_obj_set_size(s_panel, 500, 600);
    lv_obj_center(s_panel);
    lv_obj_set_style_bg_color(s_panel, lv_color_make(0x2a, 0x2a, 0x2a), 0);
    lv_obj_set_style_border_color(s_panel, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_panel, 20, 0);
    lv_obj_set_style_pad_row(s_panel, 10, 0);

    // Header
    lv_obj_t *header = lv_label_create(s_panel);
    lv_label_set_text(header, "RADAR CONFIGURATION");
    lv_obj_set_style_text_color(header, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);

    // WiFi SSID input
    lv_obj_t *ssid_label = lv_label_create(s_panel);
    lv_label_set_text(ssid_label, "WiFi SSID:");
    lv_obj_set_style_text_color(ssid_label, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_wifi_ssid_ta = lv_textarea_create(s_panel);
    lv_obj_set_width(s_wifi_ssid_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_wifi_ssid_ta, true);
    lv_textarea_set_max_length(s_wifi_ssid_ta, 63);
    lv_textarea_set_text(s_wifi_ssid_ta, s_current_config.wifi_ssid);
    lv_obj_add_event_cb(s_wifi_ssid_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_ssid_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // WiFi Password input
    lv_obj_t *password_label = lv_label_create(s_panel);
    lv_label_set_text(password_label, "WiFi Password:");
    lv_obj_set_style_text_color(password_label, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_wifi_password_ta = lv_textarea_create(s_panel);
    lv_obj_set_width(s_wifi_password_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_max_length(s_wifi_password_ta, 63);
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_textarea_set_text(s_wifi_password_ta, s_current_config.wifi_password);
    lv_obj_add_event_cb(s_wifi_password_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_password_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // Home Latitude input
    lv_obj_t *lat_label = lv_label_create(s_panel);
    lv_label_set_text(lat_label, "Home Latitude (-90 to +90):");
    lv_obj_set_style_text_color(lat_label, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_home_lat_ta = lv_textarea_create(s_panel);
    lv_obj_set_width(s_home_lat_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_home_lat_ta, true);
    lv_textarea_set_max_length(s_home_lat_ta, 15);
    char lat_buf[16];
    snprintf(lat_buf, sizeof(lat_buf), "%.6f", s_current_config.home_lat);
    lv_textarea_set_text(s_home_lat_ta, lat_buf);
    lv_obj_add_event_cb(s_home_lat_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_home_lat_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // Home Longitude input
    lv_obj_t *lon_label = lv_label_create(s_panel);
    lv_label_set_text(lon_label, "Home Longitude (-180 to +180):");
    lv_obj_set_style_text_color(lon_label, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_home_lon_ta = lv_textarea_create(s_panel);
    lv_obj_set_width(s_home_lon_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_home_lon_ta, true);
    lv_textarea_set_max_length(s_home_lon_ta, 15);
    char lon_buf[16];
    snprintf(lon_buf, sizeof(lon_buf), "%.6f", s_current_config.home_lon);
    lv_textarea_set_text(s_home_lon_ta, lon_buf);
    lv_obj_add_event_cb(s_home_lon_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_home_lon_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // Radar Radius slider
    lv_obj_t *radius_row = lv_obj_create(s_panel);
    lv_obj_set_size(radius_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(radius_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(radius_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(radius_row, 0, 0);
    lv_obj_set_style_pad_all(radius_row, 0, 0);

    lv_obj_t *radius_title = lv_label_create(radius_row);
    lv_label_set_text(radius_title, "Radar Radius:");
    lv_obj_set_style_text_color(radius_title, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_flex_grow(radius_title, 1);

    s_radius_label = lv_label_create(radius_row);
    char radius_buf[16];
    snprintf(radius_buf, sizeof(radius_buf), "%d NM", s_current_config.radar_radius_nm);
    lv_label_set_text(s_radius_label, radius_buf);
    lv_obj_set_style_text_color(s_radius_label, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_radius_slider = lv_slider_create(s_panel);
    lv_obj_set_width(s_radius_slider, LV_PCT(100));
    lv_slider_set_range(s_radius_slider, 10, 200);
    lv_slider_set_value(s_radius_slider, s_current_config.radar_radius_nm, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_radius_slider, radius_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Show Aircraft Labels checkbox
    s_show_labels_cb = lv_checkbox_create(s_panel);
    lv_checkbox_set_text(s_show_labels_cb, "Show Aircraft Labels (callsign/altitude)");
    lv_obj_set_style_text_color(s_show_labels_cb, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    if (s_current_config.show_aircraft_labels) {
        lv_obj_add_state(s_show_labels_cb, LV_STATE_CHECKED);
    }

    // Timezone dropdown
    lv_obj_t *tz_label = lv_label_create(s_panel);
    lv_label_set_text(tz_label, "Timezone:");
    lv_obj_set_style_text_color(tz_label,
        lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_timezone_dd = lv_dropdown_create(s_panel);
    lv_obj_set_width(s_timezone_dd, LV_PCT(100));
    lv_dropdown_set_options(s_timezone_dd, TIMEZONE_OPTIONS);
    lv_dropdown_set_selected(s_timezone_dd,
        timezone_offset_to_index(s_current_config.timezone_offset_hours));
    lv_obj_set_style_text_color(s_timezone_dd,
        lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);
    lv_obj_set_style_bg_color(s_timezone_dd, lv_color_make(0x40, 0x40, 0x40), 0);

    // Display Label input
    lv_obj_t *label_label = lv_label_create(s_panel);
    lv_label_set_text(label_label, "Display Label:");
    lv_obj_set_style_text_color(label_label, lv_color_make(COLOR_SWEEP_R, COLOR_SWEEP_G, COLOR_SWEEP_B), 0);

    s_display_label_ta = lv_textarea_create(s_panel);
    lv_obj_set_width(s_display_label_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_display_label_ta, true);
    lv_textarea_set_max_length(s_display_label_ta, 31);
    lv_textarea_set_text(s_display_label_ta, s_current_config.display_label);
    lv_obj_add_event_cb(s_display_label_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_display_label_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // Button row
    lv_obj_t *btn_row = lv_obj_create(s_panel);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 5, 0);

    // SAVE button
    s_save_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_save_btn, 100, 50);
    lv_obj_set_style_bg_color(s_save_btn, lv_color_make(0x00, 0xcc, 0x00), 0);
    lv_obj_t *save_label = lv_label_create(s_save_btn);
    lv_label_set_text(save_label, "SAVE");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(s_save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);

    // CANCEL button
    s_cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_cancel_btn, 100, 50);
    lv_obj_set_style_bg_color(s_cancel_btn, lv_color_make(0xcc, 0x00, 0x00), 0);
    lv_obj_t *cancel_label = lv_label_create(s_cancel_btn);
    lv_label_set_text(cancel_label, "CANCEL");
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(s_cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    // FACTORY RESET button
    s_reset_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_reset_btn, 100, 50);
    lv_obj_set_style_bg_color(s_reset_btn, lv_color_make(0xff, 0x88, 0x00), 0);
    lv_obj_t *reset_label = lv_label_create(s_reset_btn);
    lv_label_set_text(reset_label, "RESET");
    lv_obj_center(reset_label);
    lv_obj_add_event_cb(s_reset_btn, reset_btn_cb, LV_EVENT_CLICKED, NULL);

    // Create keyboard (initially hidden, sized for round screen)
    s_keyboard = lv_keyboard_create(s_overlay);
    lv_obj_set_size(s_keyboard, LV_PCT(85), LV_PCT(35));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_ready_cb, LV_EVENT_READY, NULL);

    ESP_LOGI(TAG, "Settings panel created successfully");
}

void settings_panel_close(void)
{
    close_panel();
}

bool settings_panel_is_open(void)
{
    return (s_overlay != NULL);
}

void settings_panel_set_save_callback(settings_save_callback_t callback)
{
    s_save_callback = callback;
    ESP_LOGI(TAG, "Save callback registered");
}
