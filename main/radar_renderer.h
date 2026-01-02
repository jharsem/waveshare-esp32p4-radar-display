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
 * @brief Start the radar sweep animation
 * Starts a 60Hz timer for smooth sweep rotation
 */
void radar_renderer_start_sweep(void);

/**
 * @brief Stop the radar sweep animation
 */
void radar_renderer_stop_sweep(void);

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
