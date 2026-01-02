/*
 * Aircraft Storage and Coordinate Conversion
 * Stores aircraft with computed radar positions
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Maximum aircraft to track
#define MAX_AIRCRAFT 64

// Aircraft timeout (60 seconds without update)
#define AIRCRAFT_TIMEOUT_MS 60000

// Tracked aircraft with computed radar coordinates
typedef struct {
    // Raw ADSB data
    char hex[8];           // ICAO hex code
    char callsign[12];     // Flight callsign
    float lat;             // Latitude
    float lon;             // Longitude
    int altitude;          // Altitude in feet
    float speed;           // Ground speed in knots
    float track;           // Heading in degrees

    // Computed radar position
    float distance_nm;     // Distance from home in nautical miles
    float bearing_deg;     // True bearing from home (0-360)
    int screen_x;          // Screen X coordinate (pixels)
    int screen_y;          // Screen Y coordinate (pixels)

    // Metadata
    uint32_t last_seen_ms; // Last update time (FreeRTOS ticks)
    bool active;           // Active in store
    bool has_position;     // Valid lat/lon
} tracked_aircraft_t;

/**
 * @brief Initialize aircraft store
 */
void aircraft_store_init(void);

/**
 * @brief Update aircraft from ADSB data
 * Computes distance, bearing, and screen coordinates
 * @param aircraft Array of ADSB aircraft
 * @param count Number of aircraft
 */
void aircraft_store_update(const void *aircraft, int count);

/**
 * @brief Prune stale aircraft (>60s old)
 * @return Number of aircraft removed
 */
int aircraft_store_prune(void);

/**
 * @brief Get all active aircraft for rendering
 * @param out_aircraft Output array (must hold MAX_AIRCRAFT)
 * @return Number of active aircraft
 */
int aircraft_store_get_all(tracked_aircraft_t *out_aircraft);

/**
 * @brief Get number of active aircraft
 * @return Active aircraft count
 */
int aircraft_store_get_count(void);
