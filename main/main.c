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
#include "wifi.h"
#include "radar_renderer.h"
#include "adsb_client.h"
#include "aircraft_store.h"

static const char *TAG = "main";

// LVGL display handle
static lv_display_t *s_display = NULL;

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

    // Start the radar sweep animation
    radar_renderer_start_sweep();

    bsp_display_unlock();
    ESP_LOGI(TAG, "Radar display created with sweep animation");
    log_heap_stats("after_radar");

    // Initialize aircraft store
    ESP_LOGI(TAG, "Initializing aircraft store...");
    aircraft_store_init();
    ESP_LOGI(TAG, "Aircraft store initialized");
    log_heap_stats("after_store");

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    if (!wifi_init(wifi_status_callback)) {
        ESP_LOGE(TAG, "Failed to initialize WiFi!");
        return;
    }
    ESP_LOGI(TAG, "WiFi initialization started");
    log_heap_stats("after_wifi");

    // Initialize and start ADSB client
    ESP_LOGI(TAG, "Initializing ADSB client...");
    adsb_client_init(adsb_data_callback);
    adsb_client_start();
    ESP_LOGI(TAG, "ADSB client started (polling every 10 seconds)");
    log_heap_stats("after_adsb");

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
