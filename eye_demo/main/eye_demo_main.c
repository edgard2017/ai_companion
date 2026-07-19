#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "anime_left_eye_rgb565.h"
#include "anime_right_eye_rgb565.h"

#define LCD_WIDTH 160
#define LCD_HEIGHT 160
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define LCD_BACKLIGHT_GPIO GPIO_NUM_42
#define DISPLAY_COUNT 2

#define LCD_CMD_SWRESET 0x01
#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_INVOFF 0x20
#define LCD_CMD_DISPON 0x29
#define LCD_CMD_CASET 0x2A
#define LCD_CMD_RASET 0x2B
#define LCD_CMD_RAMWR 0x2C
#define LCD_CMD_MADCTL 0x36
#define LCD_CMD_COLMOD 0x3A

typedef struct {
    uint8_t command;
    const uint8_t *data;
    size_t data_size;
    uint16_t delay_ms;
} lcd_init_command_t;

typedef struct {
    spi_host_device_t host;
    gpio_num_t mosi_gpio;
    gpio_num_t sclk_gpio;
    gpio_num_t reset_gpio;
    gpio_num_t dc_gpio;
    uint8_t orientation;
} eye_display_config_t;

static const char *TAG = "eye_demo";
static SemaphoreHandle_t frame_done_semaphores[DISPLAY_COUNT];
static esp_lcd_panel_io_handle_t panel_ios[DISPLAY_COUNT];

static const eye_display_config_t display_configs[DISPLAY_COUNT] = {
    {
        .host = SPI3_HOST,
        .mosi_gpio = GPIO_NUM_43,
        .sclk_gpio = GPIO_NUM_44,
        .reset_gpio = GPIO_NUM_46,
        .dc_gpio = GPIO_NUM_8,
        .orientation = 0x60,
    },
    {
        .host = SPI2_HOST,
        .mosi_gpio = GPIO_NUM_41,
        .sclk_gpio = GPIO_NUM_21,
        .reset_gpio = GPIO_NUM_39,
        .dc_gpio = GPIO_NUM_40,
        // Official L/R assets assume both modules use the same upright mounting.
        .orientation = 0x60,
    },
};

static const uint8_t init_80[] = {0xFF};
static const uint8_t init_81[] = {0xFF};
static const uint8_t init_82[] = {0xFF};
static const uint8_t init_84[] = {0xFF};
static const uint8_t init_85[] = {0xFF};
static const uint8_t init_86[] = {0xFF};
static const uint8_t init_87[] = {0xFF};
static const uint8_t init_88[] = {0xFF};
static const uint8_t init_89[] = {0xFF};
static const uint8_t init_8a[] = {0xFF};
static const uint8_t init_8b[] = {0xFF};
static const uint8_t init_8c[] = {0xFF};
static const uint8_t init_8d[] = {0xFF};
static const uint8_t init_8e[] = {0xFF};
static const uint8_t init_8f[] = {0xFF};
static const uint8_t init_colmod[] = {0x05};
static const uint8_t init_ec[] = {0x01};
static const uint8_t init_74[] = {0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t init_98[] = {0x3E};
static const uint8_t init_99[] = {0x3E};
static const uint8_t init_b5[] = {0x0D, 0x0D};
static const uint8_t init_60[] = {0x38, 0x0F, 0x79, 0x67};
static const uint8_t init_61[] = {0x38, 0x11, 0x79, 0x67};
static const uint8_t init_64[] = {0x38, 0x17, 0x71, 0x5F, 0x79, 0x67};
static const uint8_t init_65[] = {0x38, 0x13, 0x71, 0x5B, 0x79, 0x67};
static const uint8_t init_6a[] = {0x00, 0x00};
static const uint8_t init_6c[] = {0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50};
static const uint8_t init_6e[] = {
    0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F,
    0x0D, 0x0D, 0x0B, 0x0B, 0x09, 0x09, 0x00, 0x00,
    0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C, 0x0E, 0x0E,
    0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04,
};
static const uint8_t init_bf[] = {0x01};
static const uint8_t init_f9[] = {0x40};
static const uint8_t init_9b[] = {0x3B, 0x93, 0x33, 0x7F, 0x00};
static const uint8_t init_7e[] = {0x30};
static const uint8_t init_70[] = {0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08};
static const uint8_t init_71[] = {0x0D, 0x02, 0x08};
static const uint8_t init_91[] = {0x0E, 0x09};
static const uint8_t init_c3[] = {0x19, 0xC4, 0x19, 0xC9, 0x3C};
static const uint8_t init_f0[] = {0x53, 0x15, 0x0A, 0x04, 0x00, 0x3E};
static const uint8_t init_f1[] = {0x56, 0xA8, 0x7F, 0x33, 0x34, 0x5F};
static const uint8_t init_f2[] = {0x53, 0x15, 0x0A, 0x04, 0x00, 0x3A};
static const uint8_t init_f3[] = {0x52, 0xA4, 0x7F, 0x33, 0x34, 0xDF};

static const lcd_init_command_t lcd_init_commands[] = {
    {0xFE, NULL, 0, 0},
    {0xEF, NULL, 0, 0},
    {0x80, init_80, sizeof(init_80), 0},
    {0x81, init_81, sizeof(init_81), 0},
    {0x82, init_82, sizeof(init_82), 0},
    {0x84, init_84, sizeof(init_84), 0},
    {0x85, init_85, sizeof(init_85), 0},
    {0x86, init_86, sizeof(init_86), 0},
    {0x87, init_87, sizeof(init_87), 0},
    {0x88, init_88, sizeof(init_88), 0},
    {0x89, init_89, sizeof(init_89), 0},
    {0x8A, init_8a, sizeof(init_8a), 0},
    {0x8B, init_8b, sizeof(init_8b), 0},
    {0x8C, init_8c, sizeof(init_8c), 0},
    {0x8D, init_8d, sizeof(init_8d), 0},
    {0x8E, init_8e, sizeof(init_8e), 0},
    {0x8F, init_8f, sizeof(init_8f), 0},
    {LCD_CMD_COLMOD, init_colmod, sizeof(init_colmod), 0},
    {0xEC, init_ec, sizeof(init_ec), 0},
    {0x74, init_74, sizeof(init_74), 0},
    {0x98, init_98, sizeof(init_98), 0},
    {0x99, init_99, sizeof(init_99), 0},
    {0xB5, init_b5, sizeof(init_b5), 0},
    {0x60, init_60, sizeof(init_60), 0},
    {0x61, init_61, sizeof(init_61), 0},
    {0x64, init_64, sizeof(init_64), 0},
    {0x65, init_65, sizeof(init_65), 0},
    {0x6A, init_6a, sizeof(init_6a), 0},
    {0x6C, init_6c, sizeof(init_6c), 0},
    {0x6E, init_6e, sizeof(init_6e), 0},
    {0xBF, init_bf, sizeof(init_bf), 0},
    {0xF9, init_f9, sizeof(init_f9), 0},
    {0x9B, init_9b, sizeof(init_9b), 0},
    {0x7E, init_7e, sizeof(init_7e), 0},
    {0x70, init_70, sizeof(init_70), 0},
    {0x71, init_71, sizeof(init_71), 0},
    {0x91, init_91, sizeof(init_91), 0},
    {0xC3, init_c3, sizeof(init_c3), 0},
    {0xF0, init_f0, sizeof(init_f0), 0},
    {0xF1, init_f1, sizeof(init_f1), 0},
    {0xF2, init_f2, sizeof(init_f2), 0},
    {0xF3, init_f3, sizeof(init_f3), 0},
};

static bool IRAM_ATTR on_color_transfer_done(
    esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    SemaphoreHandle_t semaphore = (SemaphoreHandle_t)user_context;
    xSemaphoreGiveFromISR(semaphore, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static void send_command(size_t display, uint8_t command, const void *data, size_t data_size)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_ios[display], command, data, data_size));
}

static void initialize_display(size_t display)
{
    const eye_display_config_t *config = &display_configs[display];

    spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(config->host, &bus_config, SPI_DMA_CH_AUTO));

    frame_done_semaphores[display] = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(frame_done_semaphores[display] == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = config->dc_gpio,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 2,
        .on_color_trans_done = on_color_transfer_done,
        .user_ctx = frame_done_semaphores[display],
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(config->host, &io_config, &panel_ios[display]));

    gpio_set_level(config->reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(config->reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    send_command(display, LCD_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    send_command(display, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    for (size_t index = 0; index < sizeof(lcd_init_commands) / sizeof(lcd_init_commands[0]); index++) {
        const lcd_init_command_t *item = &lcd_init_commands[index];
        send_command(display, item->command, item->data, item->data_size);
        if (item->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(item->delay_ms));
        }
    }

    send_command(display, LCD_CMD_MADCTL, &config->orientation, sizeof(config->orientation));
    send_command(display, LCD_CMD_INVOFF, NULL, 0);
    send_command(display, LCD_CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void animation_state(int frame, int *offset_x, int *offset_y, int *openness)
{
    static const int8_t blink_curve[] = {100, 70, 32, 7, 32, 70, 100};

    *offset_x = 0;
    *offset_y = 0;
    *openness = 100;

    if (frame >= 55 && frame < 62) {
        *openness = blink_curve[frame - 55];
    }
}

static void render_eye(uint16_t *frame_buffer, const uint16_t *source, int frame)
{
    int offset_x;
    int offset_y;
    int openness;
    animation_state(frame, &offset_x, &offset_y, &openness);

    const uint16_t background = source[0];
    const int center_y = LCD_HEIGHT / 2;
    const int half_height = LCD_HEIGHT * openness / 200;

    for (int y = 0; y < LCD_HEIGHT; y++) {
        int relative_y = y - center_y - offset_y;
        for (int x = 0; x < LCD_WIDTH; x++) {
            int source_x = x - offset_x;
            uint16_t pixel = background;

            if (source_x >= 0 && source_x < LCD_WIDTH &&
                relative_y >= -half_height && relative_y < half_height) {
                int source_y = center_y + relative_y * 100 / openness;
                if (source_y >= 0 && source_y < LCD_HEIGHT) {
                    pixel = source[source_y * LCD_WIDTH + source_x];
                }
            }
            frame_buffer[y * LCD_WIDTH + x] = pixel;
        }
    }
}

static void display_frame(size_t display, const uint16_t *frame_buffer)
{
    const uint8_t column_range[] = {0x00, 0x00, 0x00, LCD_WIDTH - 1};
    const uint8_t row_range[] = {0x00, 0x00, 0x00, LCD_HEIGHT - 1};
    send_command(display, LCD_CMD_CASET, column_range, sizeof(column_range));
    send_command(display, LCD_CMD_RASET, row_range, sizeof(row_range));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(
        panel_ios[display],
        LCD_CMD_RAMWR,
        frame_buffer,
        LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t)));
    ESP_ERROR_CHECK(xSemaphoreTake(frame_done_semaphores[display], pdMS_TO_TICKS(1000)) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting independent dual-eye demo for Zhengchen EYE 160x160");

    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << display_configs[0].reset_gpio) |
                        (1ULL << display_configs[1].reset_gpio) |
                        (1ULL << LCD_BACKLIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));
    gpio_set_level(LCD_BACKLIGHT_GPIO, 0);
    initialize_display(0);
    initialize_display(1);

    uint16_t *frame_buffers[DISPLAY_COUNT];
    for (size_t display = 0; display < DISPLAY_COUNT; display++) {
        frame_buffers[display] = heap_caps_malloc(
            LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_ERROR_CHECK(frame_buffers[display] == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    }

    render_eye(frame_buffers[0], anime_left_eye_rgb565_wire, 0);
    render_eye(frame_buffers[1], anime_right_eye_rgb565_wire, 0);
    display_frame(0, frame_buffers[0]);
    display_frame(1, frame_buffers[1]);
    gpio_set_level(LCD_BACKLIGHT_GPIO, 1);

    int frame = 1;
    while (true) {
        render_eye(frame_buffers[0], anime_left_eye_rgb565_wire, frame);
        render_eye(frame_buffers[1], anime_right_eye_rgb565_wire, frame);
        display_frame(0, frame_buffers[0]);
        display_frame(1, frame_buffers[1]);
        frame = (frame + 1) % 70;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
