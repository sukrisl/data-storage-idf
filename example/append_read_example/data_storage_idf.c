#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_worker.h"

static const char* TAG = "data_storage_example";

static TaskHandle_t read_task_1_handle = NULL;
static TaskHandle_t read_task_2_handle = NULL;

static mount_point_t mount_cfg = {
    .base_path = "/lfs",
    .partition_label = "lfs_data",
    .auto_format_on_fail = true,
};

static storage_worker_t storage_worker = {0};

static void data_storage_cb(const storage_result_t* res, void* user_ctx) {
    if (res->op == STORAGE_OP_READ && res->user_ctx == (void*)0x2001 && read_task_1_handle != NULL) {
        xTaskNotifyGive(read_task_1_handle);
    } else if (res->op == STORAGE_OP_READ && res->user_ctx == (void*)0x2002 && read_task_2_handle != NULL) {
        xTaskNotifyGive(read_task_2_handle);
    }
}

static void read_task(void* pvParameters) {
    while (1) {
        uint8_t buffer[4096];
        memset(buffer, 0, sizeof(buffer));  // Clear buffer
        esp_err_t err = storage_read(&storage_worker, "/sensor.txt", buffer, sizeof(buffer), pvParameters);

        if (err == ESP_OK) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500))) {
                ESP_LOGI(TAG, "Read Task: Data received, content: %s", (char*)buffer);
            } else {
                ESP_LOGW(TAG, "Read Task: Timeout waiting for read completion");
            }
        }

        uint32_t random_delay = 20 + (esp_random() % 481);
        vTaskDelay(pdMS_TO_TICKS(random_delay));
    }
}

static void append_task(void* pvParameters) {
    while (1) {
        const char* data = "\nHumidity: 65%";
        storage_append(&storage_worker, "/sensor.txt", (const uint8_t*)data, strlen(data), (void*)0x3001);

        uint32_t random_delay = 100 + (esp_random() % 901);
        vTaskDelay(pdMS_TO_TICKS(random_delay));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(partition_mount(&mount_cfg));
    ESP_ERROR_CHECK(storage_init(&mount_cfg, &storage_worker, 12, 4096, 0, data_storage_cb));

    const char* data = "Temperature: 25.5C, Humidity: 60%%";
    storage_write(&storage_worker, "/sensor.txt", (const uint8_t*)data, strlen(data), NULL);

    // Create read tasks
    xTaskCreate(read_task, "read_task_1", 16384, (void*)0x2001, 5, &read_task_1_handle);
    xTaskCreate(read_task, "read_task_2", 16384, (void*)0x2002, 5, &read_task_2_handle);

    // Create append tasks
    xTaskCreate(append_task, "append_task", 4096, NULL, 5, NULL);

    vTaskDelete(NULL);
}