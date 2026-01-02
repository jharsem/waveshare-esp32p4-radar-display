/*
 * ESP32-P4 ADSB Radar Display
 * Classic rotating radar showing live aircraft within 50nm
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "radar_config.h"
#include "nvsconfig.h"
#include "settings_panel.h"
#include "wifi.h"
#include "radar_renderer.h"
#include "adsb_client.h"
#include "aircraft_store.h"

static const char *TAG = "main";

// LVGL display handle
static lv_display_t *s_display = NULL;

// Current configuration
static radar_config_t s_current_config;

// Log heap memory stats for debugging
static void log_heap_stats(const char *label)
{
    ESP_LOGI(TAG, "=== HEAP [%s] ===", label);
    ESP_LOGI(TAG, "  Internal free: %lu KB (min: %lu KB)",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    ESP_LOGI(TAG, "  SPIRAM free: %lu KB (min: %lu KB)",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

// WiFi status callback
static void wifi_status_callback(wifi_status_t status)
{
    switch (status) {
        case WIFI_STATUS_CONNECTING:
            ESP_LOGI(TAG, "WiFi: Connecting...");
            break;
        case WIFI_STATUS_CONNECTED:
            ESP_LOGI(TAG, "WiFi: Connected!");
            break;
        case WIFI_STATUS_FAILED:
            ESP_LOGE(TAG, "WiFi: Connection failed");
            break;
        default:
            break;
    }
}

// ADSB data callback
static void adsb_data_callback(const adsb_aircraft_t *aircraft, int count)
{
    ESP_LOGI(TAG, "=== Received %d aircraft from ADSB API ===", count);

    // Update aircraft store (computes distance, bearing, screen coords)
    aircraft_store_update(aircraft, count);

    // Prune stale aircraft (>60s old)
    aircraft_store_prune();

    // Get all active aircraft for rendering
    static tracked_aircraft_t display_aircraft[MAX_AIRCRAFT];
    int active_count = aircraft_store_get_all(display_aircraft);

    // Update radar display
    radar_renderer_update_aircraft(display_aircraft, active_count);

    // Log first 3 aircraft for debugging
    for (int i = 0; i < count && i < 3; i++) {
        if (aircraft[i].has_position) {
            ESP_LOGI(TAG, "  [%d] %s %-8s @ (%.4f, %.4f) alt=%d ft",
                     i,
                     aircraft[i].hex,
                     aircraft[i].callsign[0] ? aircraft[i].callsign : "N/A",
                     aircraft[i].lat,
                     aircraft[i].lon,
                     aircraft[i].altitude);
        }
    }

    if (count > 3) {
        ESP_LOGI(TAG, "  ... and %d more aircraft (%d active in store)",
                 count - 3, active_count);
    }
}

// Config button callback - open settings panel
static void config_button_callback(void)
{
    ESP_LOGI(TAG, "CONFIG button pressed - opening settings panel");

    bsp_display_lock(0);
    settings_panel_create(lv_scr_act(), &s_current_config);
    bsp_display_unlock();
}

// Settings save callback - called when user saves settings
static void on_settings_saved(const radar_config_t *new_cfg)
{
    ESP_LOGI(TAG, "Configuration updated via settings panel");

    // Save to NVS
    esp_err_t ret = nvsconfig_write_config(new_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Configuration persisted to NVS");
        nvsconfig_mark_first_boot_done();
    } else {
        ESP_LOGE(TAG, "Failed to save config to NVS: %s", esp_err_to_name(ret));
    }

    // Apply changes immediately
    bsp_display_lock(0);
    radar_renderer_set_label(new_cfg->display_label);
    radar_renderer_set_show_labels(new_cfg->show_aircraft_labels);
    bsp_display_unlock();

    aircraft_store_set_home_location(new_cfg->home_lat, new_cfg->home_lon);
    aircraft_store_set_radar_radius(new_cfg->radar_radius_nm);

    // Update ADSB client radar parameters
    adsb_client_set_radar_params(new_cfg->home_lat, new_cfg->home_lon, new_cfg->radar_radius_nm);

    // If WiFi credentials changed, reconnect
    bool wifi_changed = (strcmp(new_cfg->wifi_ssid, s_current_config.wifi_ssid) != 0 ||
                         strcmp(new_cfg->wifi_password, s_current_config.wifi_password) != 0);

    if (wifi_changed && strlen(new_cfg->wifi_ssid) > 0) {
        ESP_LOGI(TAG, "WiFi credentials changed, reconnecting...");
        // Reinitialize WiFi with new credentials
        wifi_init(new_cfg->wifi_ssid, new_cfg->wifi_password, wifi_status_callback);
    }

    // Update current config
    memcpy(&s_current_config, new_cfg, sizeof(radar_config_t));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 ADSB Radar Display Starting ===");
    log_heap_stats("startup");

    // Initialize NVS (required for WiFi)
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
    log_heap_stats("after_nvs");

    // Initialize display
    ESP_LOGI(TAG, "Initializing display...");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = SCREEN_SIZE * SCREEN_SIZE,  // Full screen buffer
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,  // CRITICAL: Use SPIRAM for large buffer
        }
    };
    s_display = bsp_display_start_with_config(&cfg);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "Failed to initialize display!");
        return;
    }
    ESP_LOGI(TAG, "Display initialized");
    log_heap_stats("after_display");

    // Set display backlight to full
    bsp_display_brightness_set(100);

    // Create radar display
    ESP_LOGI(TAG, "Creating radar display...");
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(COLOR_BACKGROUND_R,
                                                  COLOR_BACKGROUND_G,
                                                  COLOR_BACKGROUND_B), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Initialize radar renderer with distance rings
    if (!radar_renderer_init(scr)) {
        ESP_LOGE(TAG, "Failed to initialize radar renderer!");
        bsp_display_unlock();
        return;
    }

    bsp_display_unlock();
    ESP_LOGI(TAG, "Radar display created");
    log_heap_stats("after_radar");

    // Initialize aircraft store
    ESP_LOGI(TAG, "Initializing aircraft store...");
    aircraft_store_init();
    ESP_LOGI(TAG, "Aircraft store initialized");
    log_heap_stats("after_store");

    // Initialize NVS configuration module
    ESP_LOGI(TAG, "Initializing NVS configuration...");
    ret = nvsconfig_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS config!");
        return;
    }

    // Check if this is first boot or load from NVS
    if (nvsconfig_is_first_boot()) {
        ESP_LOGI(TAG, "First boot detected - using defaults");
        // Use defaults from radar_config.h
        memcpy(&s_current_config, &DEFAULT_CONFIG, sizeof(radar_config_t));
        // Clear WiFi credentials to force configuration
        s_current_config.wifi_ssid[0] = '\0';
        s_current_config.wifi_password[0] = '\0';
    } else {
        ESP_LOGI(TAG, "Loading configuration from NVS...");
        ret = nvsconfig_read_config(&s_current_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load config from NVS, using defaults");
            memcpy(&s_current_config, &DEFAULT_CONFIG, sizeof(radar_config_t));
        } else {
            ESP_LOGI(TAG, "Configuration loaded from NVS");
        }
    }

    ESP_LOGI(TAG, "Configuration ready:");
    ESP_LOGI(TAG, "  WiFi SSID: %s", s_current_config.wifi_ssid);
    ESP_LOGI(TAG, "  Home: %.4f, %.4f", s_current_config.home_lat, s_current_config.home_lon);
    ESP_LOGI(TAG, "  Radius: %d NM", s_current_config.radar_radius_nm);
    ESP_LOGI(TAG, "  Show Labels: %s", s_current_config.show_aircraft_labels ? "Yes" : "No");
    ESP_LOGI(TAG, "  Label: %s", s_current_config.display_label);
    ESP_LOGI(TAG, "  Sweep: synced to API poll interval (%.1f sec)", ADSB_POLL_INTERVAL_MS / 1000.0f);
    log_heap_stats("after_config");

    // Register settings panel callback
    settings_panel_set_save_callback(on_settings_saved);

    // Register config button callback
    radar_renderer_set_config_callback(config_button_callback);

    // Apply configuration to radar display
    bsp_display_lock(0);
    radar_renderer_set_label(s_current_config.display_label);
    // Sync sweep with API poll interval (10 seconds)
    radar_renderer_set_sweep_rate(ADSB_POLL_INTERVAL_MS / 1000.0f);
    radar_renderer_set_show_labels(s_current_config.show_aircraft_labels);
    bsp_display_unlock();

    // Start the radar sweep animation (after setting sweep rate)
    radar_renderer_start_sweep();
    ESP_LOGI(TAG, "Radar sweep started (%.1f sec per rotation)", ADSB_POLL_INTERVAL_MS / 1000.0f);

    // Apply configuration to aircraft store
    aircraft_store_set_home_location(s_current_config.home_lat, s_current_config.home_lon);
    aircraft_store_set_radar_radius(s_current_config.radar_radius_nm);

    // Check if WiFi credentials exist
    if (strlen(s_current_config.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "No WiFi credentials - opening settings panel for first-time setup");
        bsp_display_lock(0);
        settings_panel_create(lv_scr_act(), &s_current_config);
        bsp_display_unlock();
        ESP_LOGI(TAG, "Waiting for user to configure WiFi via settings panel...");
    } else {
        // Initialize WiFi with loaded configuration
        ESP_LOGI(TAG, "Initializing WiFi with saved credentials...");
        if (!wifi_init(s_current_config.wifi_ssid, s_current_config.wifi_password, wifi_status_callback)) {
            ESP_LOGE(TAG, "Failed to initialize WiFi!");
            return;
        }
        ESP_LOGI(TAG, "WiFi initialization started");
        log_heap_stats("after_wifi");

        // Initialize and start ADSB client
        ESP_LOGI(TAG, "Initializing ADSB client...");
        adsb_client_init(adsb_data_callback);
        adsb_client_set_radar_params(s_current_config.home_lat, s_current_config.home_lon, s_current_config.radar_radius_nm);
        adsb_client_start();
        ESP_LOGI(TAG, "ADSB client started (polling every 10 seconds)");
        log_heap_stats("after_adsb");
    }

    ESP_LOGI(TAG, "=== Phase 7: Aircraft Rendering Complete ===");

    // Main loop
    uint32_t loop_count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        loop_count++;

        // Log status every 10 seconds
        if (loop_count % 10 == 0) {
            ESP_LOGI(TAG, "WiFi: %s, Time synced: %s",
                     wifi_is_connected() ? "Connected" : "Disconnected",
                     wifi_is_time_synced() ? "Yes" : "No");
            log_heap_stats("periodic");
        }
    }
}
