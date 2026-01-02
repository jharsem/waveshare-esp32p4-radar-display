/*
 * WiFi Connection Module
 * Handles WiFi station mode connection and NTP time synchronization
 */

#pragma once

#include <stdbool.h>

// WiFi connection status
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED
} wifi_status_t;

// Callback for WiFi status changes
typedef void (*wifi_status_cb_t)(wifi_status_t status);

/**
 * @brief Initialize and connect to WiFi
 * @param status_cb Callback for status updates (can be NULL)
 * @return true if initialization started successfully
 */
bool wifi_init(wifi_status_cb_t status_cb);

/**
 * @brief Check if WiFi is connected
 * @return true if connected
 */
bool wifi_is_connected(void);

/**
 * @brief Check if NTP time has been synchronized
 * @return true if time is synced
 */
bool wifi_is_time_synced(void);

/**
 * @brief Get WiFi status
 * @return Current WiFi status
 */
wifi_status_t wifi_get_status(void);
