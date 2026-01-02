/*
 * NVS Configuration Storage
 * Persistent storage for radar_config_t using ESP-IDF NVS
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "radar_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize NVS for configuration storage
 *
 * Note: nvs_flash_init() must be called first in main.c
 * This function just validates NVS access
 *
 * @return ESP_OK on success
 */
esp_err_t nvsconfig_init(void);

/**
 * @brief Check if this is first boot (no config in NVS)
 *
 * @return true if first boot, false if config exists
 */
bool nvsconfig_is_first_boot(void);

/**
 * @brief Write configuration to NVS
 *
 * @param cfg Pointer to radar_config_t structure
 * @return ESP_OK on success
 */
esp_err_t nvsconfig_write_config(const radar_config_t *cfg);

/**
 * @brief Read configuration from NVS
 *
 * @param cfg Pointer to radar_config_t structure to fill
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t nvsconfig_read_config(radar_config_t *cfg);

/**
 * @brief Mark first boot as complete
 *
 * @return ESP_OK on success
 */
esp_err_t nvsconfig_mark_first_boot_done(void);

/**
 * @brief Erase all NVS data (factory reset)
 *
 * WARNING: This erases all configuration data
 *
 * @return ESP_OK on success
 */
esp_err_t nvsconfig_erase_all(void);

#ifdef __cplusplus
}
#endif
