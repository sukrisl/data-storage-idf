#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* base_path;
    const char* partition_label;
    bool auto_format_on_fail;
} mount_point_t;

esp_err_t partition_mount(const mount_point_t* cfg);
esp_err_t partition_unmount(const mount_point_t* cfg);

esp_err_t partition_get_info(const mount_point_t* cfg, size_t* out_total, size_t* out_used);

#ifdef __cplusplus
}
#endif