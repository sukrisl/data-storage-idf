#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mountable.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MAX_PATH 128
#define STORAGE_MAX_WRITE_SIZE 512
#define STORAGE_POOL_SLOTS 8

typedef enum {
    STORAGE_OP_WRITE = 0,
    STORAGE_OP_APPEND,
    STORAGE_OP_READ,
    STORAGE_OP_DELETE,
    STORAGE_OP_MKDIR,
    STORAGE_OP_RENAME,
} storage_op_t;

typedef struct {
    storage_op_t op;
    char path[STORAGE_MAX_PATH];
    esp_err_t status;
    size_t bytes_processed;  // for write/append/read
    size_t read_len;         // for read
    void* user_ctx;          // echoed from request
} storage_result_t;

typedef void (*storage_complete_cb_t)(const storage_result_t* res, void* user_ctx);

typedef struct {
    bool initialized;
    mount_point_t mount;
    TaskHandle_t task_handle;
    QueueHandle_t queue;
    storage_complete_cb_t callback;
    SemaphoreHandle_t pool_slot_mux;
    struct {
        bool used;
        uint8_t buf[STORAGE_MAX_WRITE_SIZE];
    } pool_slot[STORAGE_POOL_SLOTS];
} storage_worker_t;

esp_err_t storage_init(const mount_point_t* mount_cfg, storage_worker_t* worker, size_t queue_size,
                       uint32_t task_stack_size, uint32_t task_priority, storage_complete_cb_t callback);
esp_err_t storage_deinit(storage_worker_t* worker);

esp_err_t storage_write(storage_worker_t* worker, const char* path, const void* data, size_t len, void* user_ctx);
esp_err_t storage_append(storage_worker_t* worker, const char* path, const void* data, size_t len, void* user_ctx);
esp_err_t storage_read(storage_worker_t* worker, const char* path, void* buf, size_t buf_size, void* user_ctx);
esp_err_t storage_delete(storage_worker_t* worker, const char* path, void* user_ctx);
esp_err_t storage_mkdir(storage_worker_t* worker, const char* path, bool recursive, void* user_ctx);
esp_err_t storage_rename(storage_worker_t* worker, const char* from, const char* to, void* user_ctx);
esp_err_t storage_ls(storage_worker_t* worker, const char* path);

#ifdef __cplusplus
}
#endif