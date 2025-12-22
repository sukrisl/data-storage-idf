#include "storage_worker.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "esp_log.h"

static const char* TAG = "data_storage";

/* -------------------------------------------------------------------------- */
/* Internal Types                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    storage_op_t op;
    char path[STORAGE_MAX_PATH];

    // For write/append
    const uint8_t* data;
    size_t data_len;
    int16_t pool_index;  // -1 if not using pool

    // For read
    void* read_buf;
    size_t read_buf_size;

    // For mkdir
    bool recursive;

    // For rename
    char dest_path[STORAGE_MAX_PATH];

    void* user_ctx;
} storage_cmd_t;

typedef struct {
    bool used;
    uint8_t buf[STORAGE_MAX_WRITE_SIZE];
} storage_pool_slot_t;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t build_full_path(const mount_point_t* mount_cfg, const char* rel, char* out, size_t out_sz) {
    if (!mount_cfg || !rel || !out) return ESP_ERR_INVALID_ARG;

    int n = snprintf(out, out_sz, "%s/%s", mount_cfg->base_path, (rel[0] == '/') ? (rel + 1) : rel);

    return (n < 0 || (size_t)n >= out_sz) ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t mkdir_recursive(const char* full) {
    char tmp[STORAGE_MAX_PATH];
    strncpy(tmp, full, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';

    size_t len = strlen(tmp);
    if (len == 0) return ESP_ERR_INVALID_ARG;

    char* p = tmp + 1;

    for (; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) < 0 && errno != EEXIST) return ESP_FAIL;
            *p = '/';
        }
    }

    if (mkdir(tmp, 0775) < 0 && errno != EEXIST) return ESP_FAIL;

    return ESP_OK;
}

static esp_err_t atomic_write_file(const char* full, const uint8_t* data, size_t len, size_t* written) {
    char tmp[STORAGE_MAX_PATH + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return ESP_ERR_NO_MEM;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        ESP_LOGE(TAG, "open '%s' failed errno=%d", tmp, errno);
        return ESP_FAIL;
    }

    ssize_t w = write(fd, data, len);
    if (w < 0) {
        ESP_LOGE(TAG, "write '%s' failed errno=%d", tmp, errno);
        close(fd);
        unlink(tmp);
        return ESP_FAIL;
    }

    fsync(fd);
    close(fd);

    if (rename(tmp, full) < 0) {
        ESP_LOGE(TAG, "rename '%s' -> '%s' failed errno=%d", tmp, full, errno);
        unlink(tmp);
        return ESP_FAIL;
    }

    if (written) *written = (size_t)w;
    return ESP_OK;
}

static esp_err_t append_file(const char* full, const uint8_t* data, size_t len, size_t* written) {
    int fd = open(full, O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd < 0) {
        ESP_LOGE(TAG, "open '%s' failed errno=%d", full, errno);
        return ESP_FAIL;
    }

    ssize_t w = write(fd, data, len);
    if (w < 0) {
        ESP_LOGE(TAG, "write '%s' failed errno=%d", full, errno);
        close(fd);
        return ESP_FAIL;
    }

    fsync(fd);
    close(fd);

    if (written) *written = (size_t)w;
    return ESP_OK;
}

static esp_err_t read_file(const char* full, void* buf, size_t buf_sz, size_t* out_len) {
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open '%s' failed errno=%d", full, errno);
        return ESP_FAIL;
    }

    ssize_t r = read(fd, buf, buf_sz);
    if (r < 0) {
        ESP_LOGE(TAG, "read '%s' failed errno=%d", full, errno);
        close(fd);
        return ESP_FAIL;
    }

    close(fd);
    if (out_len) *out_len = (size_t)r;
    return ESP_OK;
}

/* Allocate a pool slot for write/append. Returns slot index or -1 if none. */
static int16_t pool_acquire_slot(storage_worker_t* worker) {
    if (!worker || !worker->pool_slot_mux) return -1;

    int16_t idx = -1;

    if (xSemaphoreTake(worker->pool_slot_mux, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < STORAGE_POOL_SLOTS; ++i) {
            if (!worker->pool_slot[i].used) {
                worker->pool_slot[i].used = true;
                idx = i;
                break;
            }
        }
        xSemaphoreGive(worker->pool_slot_mux);
    }

    return idx;
}

static void pool_release_slot(storage_worker_t* worker, int16_t idx) {
    if (!worker || !worker->pool_slot_mux || idx < 0 || idx >= STORAGE_POOL_SLOTS) return;

    if (xSemaphoreTake(worker->pool_slot_mux, portMAX_DELAY) == pdTRUE) {
        worker->pool_slot[idx].used = false;
        xSemaphoreGive(worker->pool_slot_mux);
    }
}

/* -------------------------------------------------------------------------- */
/* Storage Task                                                               */
/* -------------------------------------------------------------------------- */

static void storage_task(void* arg) {
    storage_worker_t* worker = (storage_worker_t*)arg;
    if (!worker) {
        ESP_LOGE(TAG, "storage_task: invalid worker");
        return;
    }

    storage_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(worker->queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        storage_result_t res = {
            .op = cmd.op,
            .status = ESP_OK,
            .bytes_processed = 0,
            .read_len = 0,
            .user_ctx = cmd.user_ctx,
        };
        strncpy(res.path, cmd.path, sizeof(res.path));
        res.path[sizeof(res.path) - 1] = '\0';

        char full[STORAGE_MAX_PATH + 8];
        esp_err_t err = build_full_path(&worker->mount, cmd.path, full, sizeof(full));
        if (err != ESP_OK) {
            res.status = err;
            if (worker->callback) worker->callback(&res, res.user_ctx);
            continue;
        }

        switch (cmd.op) {
            case STORAGE_OP_WRITE:
                res.status = atomic_write_file(full, cmd.data, cmd.data_len, &res.bytes_processed);
                break;

            case STORAGE_OP_APPEND:
                res.status = append_file(full, cmd.data, cmd.data_len, &res.bytes_processed);
                break;

            case STORAGE_OP_READ:
                res.status = read_file(full, cmd.read_buf, cmd.read_buf_size, &res.read_len);
                res.bytes_processed = res.read_len;
                break;

            case STORAGE_OP_DELETE:
                if (unlink(full) < 0) {
                    ESP_LOGE(TAG, "unlink '%s' failed errno=%d", full, errno);
                    res.status = ESP_FAIL;
                }
                break;

            case STORAGE_OP_MKDIR:
                if (cmd.recursive)
                    res.status = mkdir_recursive(full);
                else if (mkdir(full, 0775) < 0 && errno != EEXIST)
                    res.status = ESP_FAIL;
                break;

            case STORAGE_OP_RENAME: {
                char full_to[STORAGE_MAX_PATH + 8];
                err = build_full_path(&worker->mount, cmd.dest_path, full_to, sizeof(full_to));
                if (err != ESP_OK)
                    res.status = err;
                else if (rename(full, full_to) < 0)
                    res.status = ESP_FAIL;
                break;
            }

            default:
                res.status = ESP_ERR_INVALID_STATE;
                break;
        }

        if ((cmd.op == STORAGE_OP_WRITE || cmd.op == STORAGE_OP_APPEND) && cmd.pool_index >= 0) {
            pool_release_slot(worker, cmd.pool_index);
        }

        if (worker->callback) worker->callback(&res, res.user_ctx);
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t storage_init(const mount_point_t* mount_cfg, storage_worker_t* worker, size_t queue_size,
                       uint32_t task_stack_size, uint32_t task_priority, storage_complete_cb_t callback) {
    if (!mount_cfg || !worker) return ESP_ERR_INVALID_ARG;
    if (worker->initialized) return ESP_OK;

    worker->callback = callback;

    size_t queue_size_local = queue_size;
    if (queue_size_local == 0) {
        queue_size_local = STORAGE_POOL_SLOTS + 4;  // 8 write slots + 4 misc commands
    }

    worker->queue = xQueueCreate(queue_size_local, sizeof(storage_cmd_t));
    if (!worker->queue) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t rc =
        xTaskCreate(storage_task, "storage_task", task_stack_size, worker, task_priority, &worker->task_handle);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(worker->queue);
        worker->queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Initialize pool slots and synchronization
    worker->pool_slot_mux = xSemaphoreCreateMutex();
    if (!worker->pool_slot_mux) {
        ESP_LOGE(TAG, "semaphore create failed");
        vTaskDelete(worker->task_handle);
        worker->task_handle = NULL;
        vQueueDelete(worker->queue);
        worker->queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < STORAGE_POOL_SLOTS; ++i) {
        worker->pool_slot[i].used = false;
        // Buffers are embedded in the structure, no allocation needed
    }

    if (worker) {
        worker->mount = *mount_cfg;
    }

    worker->initialized = true;
    return ESP_OK;
}

esp_err_t storage_deinit(storage_worker_t* worker) {
    if (!worker || !worker->initialized) return ESP_OK;

    vTaskDelete(worker->task_handle);
    worker->task_handle = NULL;

    vQueueDelete(worker->queue);
    worker->queue = NULL;

    if (worker->pool_slot_mux) {
        vSemaphoreDelete(worker->pool_slot_mux);
        worker->pool_slot_mux = NULL;
    }

    worker->initialized = false;
    return ESP_OK;
}

static esp_err_t enqueue_cmd(storage_worker_t* worker, storage_cmd_t* cmd) {
    if (!worker || !cmd) return ESP_ERR_INVALID_ARG;
    if (!worker->initialized) return ESP_ERR_INVALID_STATE;

    if (xQueueSend(worker->queue, cmd, 0) != pdTRUE) {
        // If queue is full and this was a write/append, free the pool slot
        if ((cmd->op == STORAGE_OP_WRITE || cmd->op == STORAGE_OP_APPEND) && cmd->pool_index >= 0) {
            pool_release_slot(worker, cmd->pool_index);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t storage_write(storage_worker_t* worker, const char* path, const void* data, size_t len, void* ctx) {
    if (!worker || !path || !data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > STORAGE_MAX_WRITE_SIZE) return ESP_ERR_INVALID_SIZE;

    int16_t slot = pool_acquire_slot(worker);
    if (slot < 0) {
        return ESP_ERR_NO_MEM;  // all slots busy
    }

    memcpy(worker->pool_slot[slot].buf, data, len);

    storage_cmd_t cmd = {
        .op = STORAGE_OP_WRITE,
        .data = worker->pool_slot[slot].buf,
        .data_len = len,
        .pool_index = slot,
        .read_buf = NULL,
        .read_buf_size = 0,
        .recursive = false,
        .user_ctx = ctx,
    };

    strncpy(cmd.path, path, sizeof(cmd.path));
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    return enqueue_cmd(worker, &cmd);
}

esp_err_t storage_append(storage_worker_t* worker, const char* path, const void* data, size_t len, void* ctx) {
    if (!worker || !path || !data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > STORAGE_MAX_WRITE_SIZE) return ESP_ERR_INVALID_SIZE;

    int16_t slot = pool_acquire_slot(worker);
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(worker->pool_slot[slot].buf, data, len);

    storage_cmd_t cmd = {
        .op = STORAGE_OP_APPEND,
        .data = worker->pool_slot[slot].buf,
        .data_len = len,
        .pool_index = slot,
        .read_buf = NULL,
        .read_buf_size = 0,
        .recursive = false,
        .user_ctx = ctx,
    };

    strncpy(cmd.path, path, sizeof(cmd.path));
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    return enqueue_cmd(worker, &cmd);
}

esp_err_t storage_read(storage_worker_t* worker, const char* path, void* buf, size_t buf_size, void* ctx) {
    if (!worker || !path || !buf || buf_size == 0) return ESP_ERR_INVALID_ARG;

    storage_cmd_t cmd = {
        .op = STORAGE_OP_READ,
        .data = NULL,
        .data_len = 0,
        .pool_index = -1,
        .read_buf = buf,
        .read_buf_size = buf_size,
        .recursive = false,
        .user_ctx = ctx,
    };

    strncpy(cmd.path, path, sizeof(cmd.path));
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    return enqueue_cmd(worker, &cmd);
}

esp_err_t storage_delete(storage_worker_t* worker, const char* path, void* ctx) {
    if (!worker || !path) return ESP_ERR_INVALID_ARG;

    storage_cmd_t cmd = {
        .op = STORAGE_OP_DELETE,
        .data = NULL,
        .data_len = 0,
        .pool_index = -1,
        .read_buf = NULL,
        .read_buf_size = 0,
        .recursive = false,
        .user_ctx = ctx,
    };

    strncpy(cmd.path, path, sizeof(cmd.path));
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    return enqueue_cmd(worker, &cmd);
}

esp_err_t storage_mkdir(storage_worker_t* worker, const char* path, bool recursive, void* ctx) {
    if (!worker || !path) return ESP_ERR_INVALID_ARG;

    storage_cmd_t cmd = {
        .op = STORAGE_OP_MKDIR,
        .data = NULL,
        .data_len = 0,
        .pool_index = -1,
        .read_buf = NULL,
        .read_buf_size = 0,
        .recursive = recursive,
        .user_ctx = ctx,
    };

    strncpy(cmd.path, path, sizeof(cmd.path));
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    return enqueue_cmd(worker, &cmd);
}

esp_err_t storage_rename(storage_worker_t* worker, const char* from, const char* to, void* ctx) {
    if (!worker || !from || !to) return ESP_ERR_INVALID_ARG;

    storage_cmd_t cmd = {
        .op = STORAGE_OP_RENAME,
        .data = NULL,
        .data_len = 0,
        .pool_index = -1,
        .read_buf = NULL,
        .read_buf_size = 0,
        .recursive = false,
        .user_ctx = ctx,
    };

    strncpy(cmd.path, from, sizeof(cmd.path));
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    strncpy(cmd.dest_path, to, sizeof(cmd.dest_path));
    cmd.dest_path[sizeof(cmd.dest_path) - 1] = '\0';

    return enqueue_cmd(worker, &cmd);
}

esp_err_t storage_ls(storage_worker_t* worker, const char* path) {
    if (!worker || !path) return ESP_ERR_INVALID_ARG;

    char full[STORAGE_MAX_PATH + 8];
    esp_err_t err = build_full_path(&worker->mount, path, full, sizeof(full));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build_full_path('%s') failed: %s", path, esp_err_to_name(err));
        return err;
    }

    DIR* dir = opendir(full);
    if (!dir) {
        ESP_LOGE(TAG, "opendir('%s') failed errno=%d", full, errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Directory listing for: %s", full);

    struct dirent* ent;

    while ((ent = readdir(dir)) != NULL) {
        char child[STORAGE_MAX_PATH + 8];

        child[0] = '\0';
        strlcpy(child, full, sizeof(child));
        strlcat(child, "/", sizeof(child));
        strlcat(child, ent->d_name, sizeof(child));

        struct stat st;
        bool is_dir = false;
        size_t fsize = 0;

        if (stat(child, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize = is_dir ? 0 : st.st_size;
        }

        ESP_LOGI(TAG, "  %-24s  %-4s  %4u bytes", ent->d_name, is_dir ? "DIR" : "FILE", (unsigned)fsize);
    }

    closedir(dir);
    return ESP_OK;
}