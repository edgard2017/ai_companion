#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    printf("Hello from ai_companion on ESP32-S3!\n");
    printf("ESP-IDF: %s\n", esp_get_idf_version());
    printf("CPU cores: %d, silicon revision: v%d.%d\n",
           chip_info.cores,
           chip_info.revision / 100,
           chip_info.revision % 100);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash size: %" PRIu32 " MB\n", flash_size / (1024 * 1024));
    } else {
        printf("Flash size: unknown\n");
    }

    for (int seconds = 10; seconds > 0; seconds--) {
        printf("Restarting in %d seconds...\n", seconds);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}