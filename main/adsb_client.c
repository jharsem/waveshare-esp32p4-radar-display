/*
 * ADSB.lol API Client Implementation
 */

#include "adsb_client.h"
#include "radar_config.h"
#include "wifi.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "adsb_client";

// HTTP response buffer
#define HTTP_RECV_BUFFER_SIZE (32 * 1024)  // 32KB for JSON response
static char s_http_buffer[HTTP_RECV_BUFFER_SIZE];
static int s_http_buffer_len = 0;

// Client state
static adsb_data_callback_t s_data_callback = NULL;
static TaskHandle_t s_poll_task = NULL;
static bool s_running = false;
static uint32_t s_last_update_time = 0;
static int s_current_interval_ms = ADSB_POLL_INTERVAL_MS;

// Forward declarations
static void adsb_poll_task(void *pvParameters);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static bool fetch_and_parse_aircraft(void);
static int parse_aircraft_json(const char *json_str, adsb_aircraft_t *aircraft, int max_count);

void adsb_client_init(adsb_data_callback_t callback)
{
    s_data_callback = callback;
    s_http_buffer_len = 0;
    s_last_update_time = 0;
    s_current_interval_ms = ADSB_POLL_INTERVAL_MS;
    ESP_LOGI(TAG, "ADSB client initialized");
}

void adsb_client_start(void)
{
    if (s_poll_task != NULL) {
        ESP_LOGW(TAG, "ADSB client already running");
        return;
    }

    s_running = true;
    xTaskCreate(adsb_poll_task, "adsb_poll", 8192, NULL, 5, &s_poll_task);
    ESP_LOGI(TAG, "ADSB polling task started");
}

void adsb_client_stop(void)
{
    if (s_poll_task != NULL) {
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to exit
        s_poll_task = NULL;
        ESP_LOGI(TAG, "ADSB polling task stopped");
    }
}

int adsb_client_get_data_age_sec(void)
{
    if (s_last_update_time == 0) {
        return -1;  // Never updated
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    uint32_t last = s_last_update_time / 1000;
    return (int)(now - last);
}

// Internal functions

static void adsb_poll_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ADSB poll task running, waiting for WiFi...");

    while (s_running) {
        // Wait for WiFi connection
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Fetch and parse aircraft data
        ESP_LOGI(TAG, "Polling ADSB API...");
        bool success = fetch_and_parse_aircraft();

        if (success) {
            // Success - reset to base interval
            s_current_interval_ms = ADSB_POLL_INTERVAL_MS;
            s_last_update_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            ESP_LOGI(TAG, "ADSB data updated successfully, next poll in %d seconds",
                     s_current_interval_ms / 1000);
        } else {
            // Failed - exponential backoff
            s_current_interval_ms *= 2;
            if (s_current_interval_ms > ADSB_MAX_BACKOFF_MS) {
                s_current_interval_ms = ADSB_MAX_BACKOFF_MS;
            }
            ESP_LOGW(TAG, "ADSB poll failed, backing off to %d seconds",
                     s_current_interval_ms / 1000);
        }

        // Wait for next poll
        vTaskDelay(pdMS_TO_TICKS(s_current_interval_ms));
    }

    ESP_LOGI(TAG, "ADSB poll task exiting");
    vTaskDelete(NULL);
}

static bool fetch_and_parse_aircraft(void)
{
    // Build API URL
    char url[256];
    snprintf(url, sizeof(url), "%s/%.7f/%.7f/%d",
             ADSB_API_URL, HOME_LAT, HOME_LON, RADAR_RADIUS_NM);

    ESP_LOGI(TAG, "Fetching: %s", url);

    // Reset buffer
    s_http_buffer_len = 0;
    memset(s_http_buffer, 0, sizeof(s_http_buffer));

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .user_data = s_http_buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use ESP-IDF cert bundle for TLS
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    // Perform HTTP GET request
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status: %d, Length: %d bytes", status_code, s_http_buffer_len);

        if (status_code == 200 && s_http_buffer_len > 0) {
            // Parse JSON response
            adsb_aircraft_t aircraft[RADAR_MAX_AIRCRAFT];
            int count = parse_aircraft_json(s_http_buffer, aircraft, RADAR_MAX_AIRCRAFT);

            if (count > 0) {
                ESP_LOGI(TAG, "Parsed %d aircraft from API", count);

                // Call user callback
                if (s_data_callback) {
                    s_data_callback(aircraft, count);
                }

                esp_http_client_cleanup(client);
                return true;
            } else {
                ESP_LOGW(TAG, "No aircraft found in response");
            }
        } else {
            ESP_LOGW(TAG, "Bad HTTP response: status=%d, len=%d", status_code, s_http_buffer_len);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return false;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Append data to buffer
            if (s_http_buffer_len + evt->data_len < HTTP_RECV_BUFFER_SIZE - 1) {
                memcpy(s_http_buffer + s_http_buffer_len, evt->data, evt->data_len);
                s_http_buffer_len += evt->data_len;
                s_http_buffer[s_http_buffer_len] = '\0';
            } else {
                ESP_LOGW(TAG, "HTTP buffer overflow, response too large");
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP request finished");
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP disconnected");
            break;

        default:
            break;
    }
    return ESP_OK;
}

static int parse_aircraft_json(const char *json_str, adsb_aircraft_t *aircraft, int max_count)
{
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return 0;
    }

    // Get "ac" array (aircraft array)
    cJSON *ac_array = cJSON_GetObjectItem(root, "ac");
    if (!cJSON_IsArray(ac_array)) {
        ESP_LOGW(TAG, "No 'ac' array in JSON response");
        cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    cJSON *ac_item = NULL;
    cJSON_ArrayForEach(ac_item, ac_array) {
        if (count >= max_count) {
            ESP_LOGW(TAG, "Reached max aircraft limit (%d), stopping parse", max_count);
            break;
        }

        // Extract fields
        cJSON *hex = cJSON_GetObjectItem(ac_item, "hex");
        cJSON *flight = cJSON_GetObjectItem(ac_item, "flight");
        cJSON *lat = cJSON_GetObjectItem(ac_item, "lat");
        cJSON *lon = cJSON_GetObjectItem(ac_item, "lon");
        cJSON *alt_baro = cJSON_GetObjectItem(ac_item, "alt_baro");
        cJSON *gs = cJSON_GetObjectItem(ac_item, "gs");
        cJSON *track = cJSON_GetObjectItem(ac_item, "track");

        // Must have hex code and position
        if (!cJSON_IsString(hex)) {
            continue;  // Skip aircraft without hex
        }

        bool has_pos = (cJSON_IsNumber(lat) && cJSON_IsNumber(lon));

        // Fill aircraft structure
        memset(&aircraft[count], 0, sizeof(adsb_aircraft_t));

        strncpy(aircraft[count].hex, hex->valuestring, sizeof(aircraft[count].hex) - 1);

        if (cJSON_IsString(flight)) {
            // Trim whitespace from callsign
            const char *cs = flight->valuestring;
            while (*cs == ' ') cs++;  // Skip leading spaces
            strncpy(aircraft[count].callsign, cs, sizeof(aircraft[count].callsign) - 1);
            // Remove trailing spaces
            int len = strlen(aircraft[count].callsign);
            while (len > 0 && aircraft[count].callsign[len - 1] == ' ') {
                aircraft[count].callsign[--len] = '\0';
            }
        }

        if (has_pos) {
            aircraft[count].lat = (float)lat->valuedouble;
            aircraft[count].lon = (float)lon->valuedouble;
            aircraft[count].has_position = true;
        } else {
            aircraft[count].has_position = false;
        }

        aircraft[count].altitude = cJSON_IsNumber(alt_baro) ? alt_baro->valueint : 0;
        aircraft[count].speed = cJSON_IsNumber(gs) ? (float)gs->valuedouble : 0.0f;
        aircraft[count].track = cJSON_IsNumber(track) ? (float)track->valuedouble : 0.0f;

        count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Successfully parsed %d aircraft from JSON", count);
    return count;
}
