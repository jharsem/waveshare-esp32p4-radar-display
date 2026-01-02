/*
 * NVS Configuration Storage Implementation
 * Persistent storage for radar_config_t using ESP-IDF NVS
 */

#include "nvsconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "nvsconfig";

// NVS namespace for our configuration
#define NVS_NAMESPACE "radar"

// NVS keys
#define NVS_KEY_CONFIG     "config"
#define NVS_KEY_FIRST_BOOT "first_boot"

/**
 * Open NVS handle
 * Note: Caller must close with nvs_close()
 */
static esp_err_t open_nvs_handle(nvs_handle_t *out_handle)
{
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, out_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS namespace '%s': %s",
                 NVS_NAMESPACE, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t nvsconfig_init(void)
{
    // NVS must already be initialized via nvs_flash_init() in main.c
    // This function is just for validation

    nvs_handle_t handle;
    esp_err_t ret = open_nvs_handle(&handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "NVS configuration module initialized");
    return ESP_OK;
}

bool nvsconfig_is_first_boot(void)
{
    nvs_handle_t handle;
    esp_err_t ret = open_nvs_handle(&handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot determine first boot status");
        return true;  // Assume first boot on error
    }

    uint8_t first_boot = 1;
    ret = nvs_get_u8(handle, NVS_KEY_FIRST_BOOT, &first_boot);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "First boot detected (no first_boot marker in NVS)");
        return true;
    }

    return first_boot != 0;
}

esp_err_t nvsconfig_mark_first_boot_done(void)
{
    nvs_handle_t handle;
    esp_err_t ret = open_nvs_handle(&handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, NVS_KEY_FIRST_BOOT, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark first boot done: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "First boot marker set in NVS");
    }

    return ret;
}

esp_err_t nvsconfig_write_config(const radar_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_nvs_handle(&handle);
    if (ret != ESP_OK) {
        return ret;
    }

    // Write entire config structure as binary blob
    ret = nvs_set_blob(handle, NVS_KEY_CONFIG, (void *)cfg, sizeof(radar_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config blob: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // Commit changes to flash
    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Configuration written to NVS (%u bytes)",
                 sizeof(radar_config_t));
    } else {
        ESP_LOGE(TAG, "Failed to commit config to NVS: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t nvsconfig_read_config(radar_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_nvs_handle(&handle);
    if (ret != ESP_OK) {
        return ret;
    }

    // Read config blob
    size_t required_size = sizeof(radar_config_t);
    ret = nvs_get_blob(handle, NVS_KEY_CONFIG, (void *)cfg, &required_size);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Configuration read from NVS");
        return ESP_OK;
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Configuration not found in NVS");
        return ESP_ERR_NVS_NOT_FOUND;
    } else {
        ESP_LOGE(TAG, "Error reading config: %s", esp_err_to_name(ret));
        return ret;
    }
}

esp_err_t nvsconfig_erase_all(void)
{
    nvs_handle_t handle;
    esp_err_t ret = open_nvs_handle(&handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "All NVS data erased (factory reset)");
    }

    return ret;
}
