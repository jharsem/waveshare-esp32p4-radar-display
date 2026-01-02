/*
 * ADSB.lol API Client
 * Fetches live aircraft data from adsb.lol API
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Aircraft data structure from API
typedef struct {
    char hex[8];           // ICAO hex code (e.g., "7C6B2D")
    char callsign[12];     // Flight callsign (e.g., "QFA123")
    float lat;             // Latitude
    float lon;             // Longitude
    int altitude;          // Altitude in feet (barometric)
    float speed;           // Ground speed in knots
    float track;           // Heading in degrees (0-360)
    bool has_position;     // Valid lat/lon data
} adsb_aircraft_t;

// Callback for new aircraft data
// Called when API returns fresh data
typedef void (*adsb_data_callback_t)(const adsb_aircraft_t *aircraft, int count);

/**
 * @brief Initialize ADSB client
 * @param callback Callback function for new aircraft data
 */
void adsb_client_init(adsb_data_callback_t callback);

/**
 * @brief Set radar parameters (home location and radius)
 * @param lat Home latitude
 * @param lon Home longitude
 * @param radius_nm Radar radius in nautical miles
 */
void adsb_client_set_radar_params(float lat, float lon, int radius_nm);

/**
 * @brief Start the ADSB polling task
 * Polls API every 10 seconds for aircraft within configured radius
 */
void adsb_client_start(void);

/**
 * @brief Stop the ADSB polling task
 */
void adsb_client_stop(void);

/**
 * @brief Get seconds since last successful API update
 * @return Seconds since last update, or -1 if never updated
 */
int adsb_client_get_data_age_sec(void);
