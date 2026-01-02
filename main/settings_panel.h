/*
 * Settings Panel - Touch-based Configuration Overlay
 * LVGL UI for radar configuration
 */

#pragma once

#include "lvgl.h"
#include "radar_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback function type for settings save
 * Called when user taps SAVE with new configuration
 */
typedef void (*settings_save_callback_t)(const radar_config_t *new_cfg);

/**
 * @brief Create and show settings overlay panel
 *
 * Creates a semi-transparent overlay with configuration controls
 * Pauses radar sweep while panel is open
 *
 * @param parent Parent LVGL object (screen)
 * @param current_cfg Current configuration to populate fields
 */
void settings_panel_create(lv_obj_t *parent, const radar_config_t *current_cfg);

/**
 * @brief Close and destroy settings panel
 *
 * Resumes radar sweep animation
 */
void settings_panel_close(void);

/**
 * @brief Check if settings panel is currently open
 *
 * @return true if panel is visible
 */
bool settings_panel_is_open(void);

/**
 * @brief Register callback for settings save
 *
 * @param callback Function called when user saves settings
 */
void settings_panel_set_save_callback(settings_save_callback_t callback);

#ifdef __cplusplus
}
#endif
