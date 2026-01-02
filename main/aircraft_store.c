/*
 * Aircraft Storage and Coordinate Conversion Implementation
 */

#include "aircraft_store.h"
#include "adsb_client.h"
#include "radar_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char *TAG = "aircraft_store";

// Aircraft storage
static tracked_aircraft_t s_aircraft[MAX_AIRCRAFT];
static int s_active_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

// Home location (set at runtime)
static float s_home_lat = HOME_LAT;  // Default from radar_config.h
static float s_home_lon = HOME_LON;
static int s_radar_radius_nm = RADAR_RADIUS_NM;  // Default radar radius

// Forward declarations
static float haversine_distance_nm(float lat1, float lon1, float lat2, float lon2);
static float calculate_bearing(float lat1, float lon1, float lat2, float lon2);
static void polar_to_screen(float distance_nm, float bearing_deg, int *out_x, int *out_y);
static int find_aircraft(const char *hex);

void aircraft_store_init(void)
{
    memset(s_aircraft, 0, sizeof(s_aircraft));
    s_active_count = 0;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex!");
        return;
    }

    ESP_LOGI(TAG, "Aircraft store initialized (max %d aircraft)", MAX_AIRCRAFT);
}

void aircraft_store_set_home_location(float lat, float lon)
{
    s_home_lat = lat;
    s_home_lon = lon;
    ESP_LOGI(TAG, "Home location set to: %.6f, %.6f", lat, lon);
}

void aircraft_store_set_radar_radius(int radius_nm)
{
    s_radar_radius_nm = radius_nm;
    ESP_LOGI(TAG, "Radar radius set to: %d NM", radius_nm);
}

void aircraft_store_update(const void *aircraft_data, int count)
{
    const adsb_aircraft_t *aircraft = (const adsb_aircraft_t *)aircraft_data;

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Store not initialized!");
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int updated = 0;
    int new_aircraft = 0;

    for (int i = 0; i < count; i++) {
        if (!aircraft[i].has_position) {
            continue;  // Skip aircraft without position
        }

        // Find existing or new slot
        int idx = find_aircraft(aircraft[i].hex);
        if (idx == -1) {
            // Find empty slot
            for (int j = 0; j < MAX_AIRCRAFT; j++) {
                if (!s_aircraft[j].active) {
                    idx = j;
                    new_aircraft++;
                    break;
                }
            }
        } else {
            updated++;
        }

        if (idx == -1) {
            ESP_LOGW(TAG, "No free slots for aircraft %s", aircraft[i].hex);
            continue;
        }

        // Copy raw data
        strncpy(s_aircraft[idx].hex, aircraft[i].hex, sizeof(s_aircraft[idx].hex) - 1);
        strncpy(s_aircraft[idx].callsign, aircraft[i].callsign, sizeof(s_aircraft[idx].callsign) - 1);
        s_aircraft[idx].lat = aircraft[i].lat;
        s_aircraft[idx].lon = aircraft[i].lon;
        s_aircraft[idx].altitude = aircraft[i].altitude;
        s_aircraft[idx].speed = aircraft[i].speed;
        s_aircraft[idx].track = aircraft[i].track;
        s_aircraft[idx].has_position = true;

        // Compute distance and bearing
        s_aircraft[idx].distance_nm = haversine_distance_nm(
            s_home_lat, s_home_lon,
            aircraft[i].lat, aircraft[i].lon
        );

        s_aircraft[idx].bearing_deg = calculate_bearing(
            s_home_lat, s_home_lon,
            aircraft[i].lat, aircraft[i].lon
        );

        // Convert to screen coordinates
        polar_to_screen(
            s_aircraft[idx].distance_nm,
            s_aircraft[idx].bearing_deg,
            &s_aircraft[idx].screen_x,
            &s_aircraft[idx].screen_y
        );

        // Update metadata
        s_aircraft[idx].last_seen_ms = now;
        s_aircraft[idx].active = true;
    }

    // Update active count
    s_active_count = 0;
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (s_aircraft[i].active) {
            s_active_count++;
        }
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Updated %d aircraft, %d new, %d total active",
             updated, new_aircraft, s_active_count);
}

int aircraft_store_prune(void)
{
    if (s_mutex == NULL) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int pruned = 0;

    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (s_aircraft[i].active) {
            uint32_t age_ms = now - s_aircraft[i].last_seen_ms;
            if (age_ms > AIRCRAFT_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Pruning stale aircraft %s (age: %lu ms)",
                         s_aircraft[i].hex, age_ms);
                s_aircraft[i].active = false;
                pruned++;
            }
        }
    }

    // Update active count
    s_active_count = 0;
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (s_aircraft[i].active) {
            s_active_count++;
        }
    }

    xSemaphoreGive(s_mutex);

    if (pruned > 0) {
        ESP_LOGI(TAG, "Pruned %d stale aircraft, %d remain", pruned, s_active_count);
    }

    return pruned;
}

int aircraft_store_get_all(tracked_aircraft_t *out_aircraft)
{
    if (s_mutex == NULL || out_aircraft == NULL) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int count = 0;
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (s_aircraft[i].active) {
            memcpy(&out_aircraft[count], &s_aircraft[i], sizeof(tracked_aircraft_t));
            count++;
        }
    }

    xSemaphoreGive(s_mutex);

    return count;
}

int aircraft_store_get_count(void)
{
    return s_active_count;
}

// Internal functions

static int find_aircraft(const char *hex)
{
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (s_aircraft[i].active && strcmp(s_aircraft[i].hex, hex) == 0) {
            return i;
        }
    }
    return -1;
}

static float haversine_distance_nm(float lat1, float lon1, float lat2, float lon2)
{
    // Haversine formula for great circle distance
    // Returns distance in nautical miles

    const float R_NM = 3440.065f;  // Earth radius in nautical miles

    float lat1_rad = lat1 * M_PI / 180.0f;
    float lat2_rad = lat2 * M_PI / 180.0f;
    float dlat = (lat2 - lat1) * M_PI / 180.0f;
    float dlon = (lon2 - lon1) * M_PI / 180.0f;

    float a = sinf(dlat / 2.0f) * sinf(dlat / 2.0f) +
              cosf(lat1_rad) * cosf(lat2_rad) *
              sinf(dlon / 2.0f) * sinf(dlon / 2.0f);

    float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));

    return R_NM * c;
}

static float calculate_bearing(float lat1, float lon1, float lat2, float lon2)
{
    // Calculate true bearing (0-360 degrees, 0 = North)

    float lat1_rad = lat1 * M_PI / 180.0f;
    float lat2_rad = lat2 * M_PI / 180.0f;
    float dlon_rad = (lon2 - lon1) * M_PI / 180.0f;

    float y = sinf(dlon_rad) * cosf(lat2_rad);
    float x = cosf(lat1_rad) * sinf(lat2_rad) -
              sinf(lat1_rad) * cosf(lat2_rad) * cosf(dlon_rad);

    float bearing_rad = atan2f(y, x);
    float bearing_deg = bearing_rad * 180.0f / M_PI;

    // Normalize to 0-360
    bearing_deg = fmodf(bearing_deg + 360.0f, 360.0f);

    return bearing_deg;
}

static void polar_to_screen(float distance_nm, float bearing_deg, int *out_x, int *out_y)
{
    // Convert polar coordinates (distance, bearing) to screen coordinates
    // Bearing: 0° = North, increases clockwise
    // Screen: (0,0) = top-left, X right, Y down

    // Pixels per nautical mile (using runtime radar radius)
    const float pixels_per_nm = (float)RADAR_DISPLAY_RADIUS / (float)s_radar_radius_nm;

    // Calculate radius in pixels
    float radius_px = distance_nm * pixels_per_nm;

    // Convert bearing to radians and adjust for screen coords
    // Subtract 90° to rotate North to top, then negate for clockwise
    float angle_rad = (bearing_deg - 90.0f) * M_PI / 180.0f;

    // Calculate screen position relative to center
    int dx = (int)(radius_px * cosf(angle_rad));
    int dy = (int)(radius_px * sinf(angle_rad));

    // Convert to absolute screen coordinates
    *out_x = SCREEN_CENTER_X + dx;
    *out_y = SCREEN_CENTER_Y + dy;
}
