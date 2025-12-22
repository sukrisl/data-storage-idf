#include "mountable.h"
#include <string.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char* TAG = "partition_mount";

esp_err_t partition_mount(const mount_point_t* cfg) {
    if (!cfg || !cfg->base_path || !cfg->partition_label) return ESP_ERR_INVALID_ARG;

    char base_path[16];
    strncpy(base_path, cfg->base_path, sizeof(base_path));
    base_path[sizeof(base_path) - 1] = '\0';

    char partition[16];
    strncpy(partition, cfg->partition_label, sizeof(partition));
    partition[sizeof(partition) - 1] = '\0';

    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = partition,
        .format_if_mount_failed = cfg->auto_format_on_fail,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t partition_unmount(const mount_point_t* cfg) {
    if (!cfg || !cfg->partition_label) return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_vfs_littlefs_unregister(cfg->partition_label);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_unregister failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t partition_get_info(const mount_point_t* cfg, size_t* out_total,
                             size_t* out_used) {
    if (!cfg) return ESP_ERR_INVALID_ARG;

    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(cfg->partition_label, &total, &used);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_littlefs_info failed: %s", esp_err_to_name(err));
        return err;
    }

    if (out_total) *out_total = total;
    if (out_used) *out_used = used;

    return ESP_OK;
}