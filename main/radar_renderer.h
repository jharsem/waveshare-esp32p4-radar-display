/*
 * Radar Display Renderer
 * Renders the classic PPI radar scope with distance rings and sweep
 */

#pragma once

#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief Initialize radar renderer and create static display elements
 * @param parent Parent LVGL object (screen)
 * @return true if initialization successful
 */
bool radar_renderer_init(lv_obj_t *parent);

/**
 * @brief Get the radar screen container object
 * @return Radar container object (for adding aircraft)
 */
lv_obj_t *radar_renderer_get_container(void);

/**
 * @brief Set the display label shown at the top of the radar
 * @param label Label text (e.g., "RADAR - 50NM")
 */
void radar_renderer_set_label(const char *label);

/**
 * @brief Set the sweep rotation rate
 * @param sweep_seconds Seconds for one full 360° rotation (e.g., 10.0)
 */
void radar_renderer_set_sweep_rate(float sweep_seconds);

/**
 * @brief Set whether to show aircraft labels (callsign/altitude)
 * @param show_labels true to show labels, false to hide
 */
void radar_renderer_set_show_labels(bool show_labels);

/**
 * @brief Callback function type for config button press
 */
typedef void (*config_button_callback_t)(void);

/**
 * @brief Register callback for config button press
 * @param callback Function called when config button is clicked
 */
void radar_renderer_set_config_callback(config_button_callback_t callback);

/**
 * @brief Start the radar sweep animation
 * Starts a 60Hz timer for smooth sweep rotation
 */
void radar_renderer_start_sweep(void);

/**
 * @brief Stop the radar sweep animation
 */
void radar_renderer_stop_sweep(void);

/**
 * @brief Pause the radar sweep animation (for config panel)
 */
void radar_renderer_pause_sweep(void);

/**
 * @brief Resume the radar sweep animation
 */
void radar_renderer_resume_sweep(void);

/**
 * @brief Toggle debug overlay (shows center point and ring radii)
 * @param enable true to show debug overlay
 */
void radar_renderer_debug_overlay(bool enable);

/**
 * @brief Update aircraft blips on radar display
 * Creates/updates aircraft markers (blips + labels) from aircraft store
 * @param aircraft Array of tracked aircraft
 * @param count Number of aircraft
 */
void radar_renderer_update_aircraft(const void *aircraft, int count);

/**
 * @brief Start the clock display timer
 * Updates clock every 1 second with current UTC time (adjusted by timezone offset)
 */
void radar_renderer_start_clock(void);

/**
 * @brief Set timezone offset for clock display
 * @param offset_hours Hours offset from UTC (-12 to +14)
 */
void radar_renderer_set_timezone(int8_t offset_hours);
