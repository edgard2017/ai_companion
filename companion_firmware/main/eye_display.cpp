#include "eye_display.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "eye_neutral_rgb565.h"

namespace companion {
namespace {

constexpr int kLcdWidth = 160;
constexpr int kLcdHeight = 160;
constexpr std::size_t kFrameBytes =
    kLcdWidth * kLcdHeight * sizeof(std::uint16_t);
constexpr int kPixelClockHz = 40 * 1000 * 1000;
constexpr TickType_t kFramePeriod = pdMS_TO_TICKS(80);

// The two LCD modules share this one signal chain.  Do not initialize SPI2 or
// attempt to address a second panel: this board cannot show independent frames.
constexpr spi_host_device_t kLcdHost = SPI3_HOST;
constexpr gpio_num_t kLcdMosiGpio = GPIO_NUM_43;
constexpr gpio_num_t kLcdClockGpio = GPIO_NUM_44;
constexpr gpio_num_t kLcdResetGpio = GPIO_NUM_46;
constexpr gpio_num_t kLcdDcGpio = GPIO_NUM_8;
constexpr gpio_num_t kLcdBacklightGpio = GPIO_NUM_42;
constexpr std::uint8_t kLcdOrientation = 0x60;

constexpr std::uint8_t kCmdSoftwareReset = 0x01;
constexpr std::uint8_t kCmdSleepOut = 0x11;
constexpr std::uint8_t kCmdInversionOff = 0x20;
constexpr std::uint8_t kCmdDisplayOn = 0x29;
constexpr std::uint8_t kCmdColumnAddress = 0x2A;
constexpr std::uint8_t kCmdRowAddress = 0x2B;
constexpr std::uint8_t kCmdMemoryWrite = 0x2C;
constexpr std::uint8_t kCmdMemoryAccessControl = 0x36;
constexpr std::uint8_t kCmdPixelFormat = 0x3A;

struct LcdInitCommand {
    std::uint8_t command;
    const std::uint8_t *data;
    std::size_t data_size;
    std::uint16_t delay_ms;
};

struct Rgb {
    int red;
    int green;
    int blue;
};

struct FrameStyle {
    int offset_x = 0;
    int offset_y = 0;
    int openness = 100;
    int brightness = 100;
    Rgb tint = {0, 0, 0};
    int tint_percent = 0;
    bool ring = false;
    Rgb ring_color = {0, 0, 0};
    int ring_percent = 100;
    bool thinking_dots = false;
};

const char *const kTag = "eye_display";

std::atomic<EyeState> g_state{EyeState::kIdle};
std::atomic<bool> g_initialization_started{false};
esp_lcd_panel_io_handle_t g_panel_io = nullptr;
SemaphoreHandle_t g_frame_done = nullptr;
TaskHandle_t g_animation_task = nullptr;
std::uint16_t *g_frame_buffer = nullptr;
bool g_spi_bus_initialized = false;

const std::uint8_t kInit80[] = {0xFF};
const std::uint8_t kInit81[] = {0xFF};
const std::uint8_t kInit82[] = {0xFF};
const std::uint8_t kInit84[] = {0xFF};
const std::uint8_t kInit85[] = {0xFF};
const std::uint8_t kInit86[] = {0xFF};
const std::uint8_t kInit87[] = {0xFF};
const std::uint8_t kInit88[] = {0xFF};
const std::uint8_t kInit89[] = {0xFF};
const std::uint8_t kInit8a[] = {0xFF};
const std::uint8_t kInit8b[] = {0xFF};
const std::uint8_t kInit8c[] = {0xFF};
const std::uint8_t kInit8d[] = {0xFF};
const std::uint8_t kInit8e[] = {0xFF};
const std::uint8_t kInit8f[] = {0xFF};
const std::uint8_t kInitPixelFormat[] = {0x05};
const std::uint8_t kInitEc[] = {0x01};
const std::uint8_t kInit74[] = {0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00};
const std::uint8_t kInit98[] = {0x3E};
const std::uint8_t kInit99[] = {0x3E};
const std::uint8_t kInitB5[] = {0x0D, 0x0D};
const std::uint8_t kInit60[] = {0x38, 0x0F, 0x79, 0x67};
const std::uint8_t kInit61[] = {0x38, 0x11, 0x79, 0x67};
const std::uint8_t kInit64[] = {0x38, 0x17, 0x71, 0x5F, 0x79, 0x67};
const std::uint8_t kInit65[] = {0x38, 0x13, 0x71, 0x5B, 0x79, 0x67};
const std::uint8_t kInit6a[] = {0x00, 0x00};
const std::uint8_t kInit6c[] = {0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50};
const std::uint8_t kInit6e[] = {
    0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F,
    0x0D, 0x0D, 0x0B, 0x0B, 0x09, 0x09, 0x00, 0x00,
    0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C, 0x0E, 0x0E,
    0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04,
};
const std::uint8_t kInitBf[] = {0x01};
const std::uint8_t kInitF9[] = {0x40};
const std::uint8_t kInit9b[] = {0x3B, 0x93, 0x33, 0x7F, 0x00};
const std::uint8_t kInit7e[] = {0x30};
const std::uint8_t kInit70[] = {0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08};
const std::uint8_t kInit71[] = {0x0D, 0x02, 0x08};
const std::uint8_t kInit91[] = {0x0E, 0x09};
const std::uint8_t kInitC3[] = {0x19, 0xC4, 0x19, 0xC9, 0x3C};
const std::uint8_t kInitF0[] = {0x53, 0x15, 0x0A, 0x04, 0x00, 0x3E};
const std::uint8_t kInitF1[] = {0x56, 0xA8, 0x7F, 0x33, 0x34, 0x5F};
const std::uint8_t kInitF2[] = {0x53, 0x15, 0x0A, 0x04, 0x00, 0x3A};
const std::uint8_t kInitF3[] = {0x52, 0xA4, 0x7F, 0x33, 0x34, 0xDF};

const LcdInitCommand kLcdInitCommands[] = {
    {0xFE, nullptr, 0, 0},
    {0xEF, nullptr, 0, 0},
    {0x80, kInit80, sizeof(kInit80), 0},
    {0x81, kInit81, sizeof(kInit81), 0},
    {0x82, kInit82, sizeof(kInit82), 0},
    {0x84, kInit84, sizeof(kInit84), 0},
    {0x85, kInit85, sizeof(kInit85), 0},
    {0x86, kInit86, sizeof(kInit86), 0},
    {0x87, kInit87, sizeof(kInit87), 0},
    {0x88, kInit88, sizeof(kInit88), 0},
    {0x89, kInit89, sizeof(kInit89), 0},
    {0x8A, kInit8a, sizeof(kInit8a), 0},
    {0x8B, kInit8b, sizeof(kInit8b), 0},
    {0x8C, kInit8c, sizeof(kInit8c), 0},
    {0x8D, kInit8d, sizeof(kInit8d), 0},
    {0x8E, kInit8e, sizeof(kInit8e), 0},
    {0x8F, kInit8f, sizeof(kInit8f), 0},
    {kCmdPixelFormat, kInitPixelFormat, sizeof(kInitPixelFormat), 0},
    {0xEC, kInitEc, sizeof(kInitEc), 0},
    {0x74, kInit74, sizeof(kInit74), 0},
    {0x98, kInit98, sizeof(kInit98), 0},
    {0x99, kInit99, sizeof(kInit99), 0},
    {0xB5, kInitB5, sizeof(kInitB5), 0},
    {0x60, kInit60, sizeof(kInit60), 0},
    {0x61, kInit61, sizeof(kInit61), 0},
    {0x64, kInit64, sizeof(kInit64), 0},
    {0x65, kInit65, sizeof(kInit65), 0},
    {0x6A, kInit6a, sizeof(kInit6a), 0},
    {0x6C, kInit6c, sizeof(kInit6c), 0},
    {0x6E, kInit6e, sizeof(kInit6e), 0},
    {0xBF, kInitBf, sizeof(kInitBf), 0},
    {0xF9, kInitF9, sizeof(kInitF9), 0},
    {0x9B, kInit9b, sizeof(kInit9b), 0},
    {0x7E, kInit7e, sizeof(kInit7e), 0},
    {0x70, kInit70, sizeof(kInit70), 0},
    {0x71, kInit71, sizeof(kInit71), 0},
    {0x91, kInit91, sizeof(kInit91), 0},
    {0xC3, kInitC3, sizeof(kInitC3), 0},
    {0xF0, kInitF0, sizeof(kInitF0), 0},
    {0xF1, kInitF1, sizeof(kInitF1), 0},
    {0xF2, kInitF2, sizeof(kInitF2), 0},
    {0xF3, kInitF3, sizeof(kInitF3), 0},
};

int clamp_channel(int value, int maximum)
{
    if (value < 0) {
        return 0;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

std::uint16_t swap_bytes(std::uint16_t value)
{
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

Rgb decode_rgb565_wire(std::uint16_t wire_pixel)
{
    const std::uint16_t pixel = swap_bytes(wire_pixel);
    return {
        static_cast<int>((pixel >> 11) & 0x1F),
        static_cast<int>((pixel >> 5) & 0x3F),
        static_cast<int>(pixel & 0x1F),
    };
}

std::uint16_t encode_rgb565_wire(const Rgb &color)
{
    const int red = clamp_channel(color.red, 31);
    const int green = clamp_channel(color.green, 63);
    const int blue = clamp_channel(color.blue, 31);
    const std::uint16_t pixel = static_cast<std::uint16_t>(
        (red << 11) | (green << 5) | blue);
    return swap_bytes(pixel);
}

Rgb styled_color(Rgb source, const FrameStyle &style)
{
    if (style.tint_percent > 0) {
        source.red = (source.red * (100 - style.tint_percent) +
                      style.tint.red * style.tint_percent) /
                     100;
        source.green = (source.green * (100 - style.tint_percent) +
                        style.tint.green * style.tint_percent) /
                       100;
        source.blue = (source.blue * (100 - style.tint_percent) +
                       style.tint.blue * style.tint_percent) /
                      100;
    }
    source.red = source.red * style.brightness / 100;
    source.green = source.green * style.brightness / 100;
    source.blue = source.blue * style.brightness / 100;
    return source;
}

int triangle_wave(std::uint32_t frame, int period, int amplitude)
{
    if (period < 2 || amplitude <= 0) {
        return 0;
    }
    const int phase = static_cast<int>(frame % static_cast<std::uint32_t>(period));
    const int half = period / 2;
    if (phase <= half) {
        return phase * amplitude / half;
    }
    return (period - phase) * amplitude / (period - half);
}

int natural_blink(std::uint32_t frame, int period)
{
    constexpr int kBlinkFrames = 7;
    constexpr int kBlinkCurve[kBlinkFrames] = {100, 70, 30, 8, 30, 70, 100};
    const int phase = static_cast<int>(frame % static_cast<std::uint32_t>(period));
    if (phase < period - kBlinkFrames) {
        return 100;
    }
    return kBlinkCurve[phase - (period - kBlinkFrames)];
}

FrameStyle style_for(EyeState state, std::uint32_t frame)
{
    FrameStyle style;
    switch (state) {
        case EyeState::kIdle:
            style.openness = natural_blink(frame, 56);
            style.brightness = 96 + triangle_wave(frame, 32, 4);
            break;
        case EyeState::kListening:
            style.brightness = 102 + triangle_wave(frame, 16, 13);
            style.tint = {0, 63, 31};
            style.tint_percent = 7;
            style.ring = true;
            style.ring_color = {0, 63, 31};
            style.ring_percent = 45 + triangle_wave(frame, 16, 45);
            break;
        case EyeState::kThinking: {
            const int sweep = triangle_wave(frame, 28, 10) - 5;
            style.offset_x = sweep;
            style.openness = natural_blink(frame, 69);
            style.brightness = 92;
            style.tint = {20, 4, 31};
            style.tint_percent = 13;
            style.thinking_dots = true;
            break;
        }
        case EyeState::kSpeaking:
            style.offset_y = triangle_wave(frame, 12, 2) - 1;
            style.openness = 91 + triangle_wave(frame, 12, 9);
            style.brightness = 100 + triangle_wave(frame, 12, 15);
            style.tint = {0, 63, 12};
            style.tint_percent = 8;
            style.ring = true;
            style.ring_color = {2, 63, 15};
            style.ring_percent = 35 + triangle_wave(frame, 12, 35);
            break;
        case EyeState::kDictation:
            style.openness = natural_blink(frame, 62);
            style.brightness = 98 + triangle_wave(frame, 24, 6);
            style.tint = {31, 36, 0};
            style.tint_percent = 8;
            style.ring = true;
            style.ring_color = {31, 40, 0};
            style.ring_percent = 48 + triangle_wave(frame, 24, 35);
            break;
        case EyeState::kError:
            style.openness = 84;
            style.brightness = ((frame / 5U) % 2U == 0U) ? 100 : 62;
            style.tint = {31, 0, 0};
            style.tint_percent = 48;
            style.ring = true;
            style.ring_color = {31, 0, 0};
            style.ring_percent = 100;
            break;
    }
    return style;
}

bool is_ring_pixel(int x, int y)
{
    const int dx = x - kLcdWidth / 2;
    const int dy = y - kLcdHeight / 2;
    const int radius_squared = dx * dx + dy * dy;
    return radius_squared >= 73 * 73 && radius_squared <= 76 * 76;
}

void render_frame(EyeState state, std::uint32_t frame)
{
    const FrameStyle style = style_for(state, frame);
    const std::uint16_t background = eye_neutral_rgb565_wire[0];
    const int center_y = kLcdHeight / 2;
    const int half_height = kLcdHeight * style.openness / 200;

    for (int y = 0; y < kLcdHeight; ++y) {
        const int relative_y = y - center_y - style.offset_y;
        for (int x = 0; x < kLcdWidth; ++x) {
            const int source_x = x - style.offset_x;
            std::uint16_t source_pixel = background;
            if (source_x >= 0 && source_x < kLcdWidth &&
                relative_y >= -half_height && relative_y < half_height) {
                const int source_y =
                    center_y + relative_y * 100 / style.openness;
                if (source_y >= 0 && source_y < kLcdHeight) {
                    source_pixel =
                        eye_neutral_rgb565_wire[source_y * kLcdWidth + source_x];
                }
            }

            Rgb color = styled_color(decode_rgb565_wire(source_pixel), style);
            if (style.ring && is_ring_pixel(x, y)) {
                color.red = (color.red * (100 - style.ring_percent) +
                             style.ring_color.red * style.ring_percent) /
                            100;
                color.green = (color.green * (100 - style.ring_percent) +
                               style.ring_color.green * style.ring_percent) /
                              100;
                color.blue = (color.blue * (100 - style.ring_percent) +
                              style.ring_color.blue * style.ring_percent) /
                             100;
            }

            // Three small moving purple dots make "thinking" unmistakable while
            // keeping the original eye art visible.
            if (style.thinking_dots && y >= 139 && y <= 143) {
                const int active_dot = static_cast<int>((frame / 4U) % 3U);
                for (int dot = 0; dot < 3; ++dot) {
                    const int dot_x = 68 + dot * 12;
                    const int dx = x - dot_x;
                    const int dy = y - 141;
                    if (dx * dx + dy * dy <= 5) {
                        color = (dot == active_dot) ? Rgb{25, 16, 31}
                                                    : Rgb{8, 5, 11};
                    }
                }
            }
            g_frame_buffer[y * kLcdWidth + x] = encode_rgb565_wire(color);
        }
    }
}

bool IRAM_ATTR on_color_transfer_done(
    esp_lcd_panel_io_handle_t,
    esp_lcd_panel_io_event_data_t *,
    void *user_context)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(
        static_cast<SemaphoreHandle_t>(user_context),
        &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

esp_err_t send_command(
    std::uint8_t command,
    const void *data = nullptr,
    std::size_t data_size = 0)
{
    return esp_lcd_panel_io_tx_param(g_panel_io, command, data, data_size);
}

void release_lcd_resources()
{
    gpio_set_level(kLcdBacklightGpio, 0);
    if (g_panel_io != nullptr) {
        esp_lcd_panel_io_del(g_panel_io);
        g_panel_io = nullptr;
    }
    if (g_frame_done != nullptr) {
        vSemaphoreDelete(g_frame_done);
        g_frame_done = nullptr;
    }
    if (g_spi_bus_initialized) {
        spi_bus_free(kLcdHost);
        g_spi_bus_initialized = false;
    }
}

esp_err_t initialize_lcd_controller()
{
    gpio_config_t output_config = {};
    output_config.pin_bit_mask =
        (1ULL << kLcdResetGpio) | (1ULL << kLcdBacklightGpio);
    output_config.mode = GPIO_MODE_OUTPUT;
    output_config.pull_up_en = GPIO_PULLUP_DISABLE;
    output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    output_config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t error = gpio_config(&output_config);
    if (error != ESP_OK) {
        return error;
    }
    gpio_set_level(kLcdBacklightGpio, 0);

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = kLcdMosiGpio;
    bus_config.miso_io_num = GPIO_NUM_NC;
    bus_config.sclk_io_num = kLcdClockGpio;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = kFrameBytes;
    error = spi_bus_initialize(kLcdHost, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        return error;
    }
    g_spi_bus_initialized = true;

    g_frame_done = xSemaphoreCreateBinary();
    if (g_frame_done == nullptr) {
        release_lcd_resources();
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = GPIO_NUM_NC;
    io_config.dc_gpio_num = kLcdDcGpio;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = 1;
    io_config.on_color_trans_done = on_color_transfer_done;
    io_config.user_ctx = g_frame_done;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    error = esp_lcd_new_panel_io_spi(kLcdHost, &io_config, &g_panel_io);
    if (error != ESP_OK) {
        release_lcd_resources();
        return error;
    }

    gpio_set_level(kLcdResetGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kLcdResetGpio, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if ((error = send_command(kCmdSoftwareReset)) != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if ((error = send_command(kCmdSleepOut)) != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    for (const LcdInitCommand &item : kLcdInitCommands) {
        error = send_command(item.command, item.data, item.data_size);
        if (error != ESP_OK) {
            return error;
        }
        if (item.delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(item.delay_ms));
        }
    }

    if ((error = send_command(
             kCmdMemoryAccessControl,
             &kLcdOrientation,
             sizeof(kLcdOrientation))) != ESP_OK) {
        return error;
    }
    if ((error = send_command(kCmdInversionOff)) != ESP_OK) {
        return error;
    }
    if ((error = send_command(kCmdDisplayOn)) != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t display_frame()
{
    const std::uint8_t column_range[] = {
        0x00, 0x00, 0x00, static_cast<std::uint8_t>(kLcdWidth - 1)};
    const std::uint8_t row_range[] = {
        0x00, 0x00, 0x00, static_cast<std::uint8_t>(kLcdHeight - 1)};

    esp_err_t error =
        send_command(kCmdColumnAddress, column_range, sizeof(column_range));
    if (error != ESP_OK) {
        return error;
    }
    error = send_command(kCmdRowAddress, row_range, sizeof(row_range));
    if (error != ESP_OK) {
        return error;
    }

    // Drain a stale completion signal before submitting the sole DMA buffer.
    xSemaphoreTake(g_frame_done, 0);
    error = esp_lcd_panel_io_tx_color(
        g_panel_io, kCmdMemoryWrite, g_frame_buffer, kFrameBytes);
    if (error != ESP_OK) {
        return error;
    }
    return xSemaphoreTake(g_frame_done, pdMS_TO_TICKS(1000)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

const char *state_name(EyeState state)
{
    switch (state) {
        case EyeState::kIdle:
            return "idle";
        case EyeState::kListening:
            return "listening";
        case EyeState::kThinking:
            return "thinking";
        case EyeState::kSpeaking:
            return "speaking";
        case EyeState::kDictation:
            return "dictation";
        case EyeState::kError:
            return "error";
    }
    return "unknown";
}

void animation_task(void *)
{
    EyeState active_state = g_state.load(std::memory_order_relaxed);
    std::uint32_t frame = 0;
    TickType_t next_frame = xTaskGetTickCount();

    while (true) {
        const EyeState requested_state = g_state.load(std::memory_order_relaxed);
        if (requested_state != active_state) {
            active_state = requested_state;
            frame = 0;
            ESP_LOGI(kTag, "state=%s", state_name(active_state));
        }
        render_frame(active_state, frame++);
        const esp_err_t error = display_frame();
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "frame transfer failed: %s", esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(400));
            next_frame = xTaskGetTickCount();
        }
        vTaskDelayUntil(&next_frame, kFramePeriod);
    }
}

}  // namespace

esp_err_t eye_display_init()
{
    if (g_initialization_started.exchange(true, std::memory_order_acq_rel)) {
        return ESP_OK;
    }

    ESP_LOGI(
        kTag,
        "initializing one SPI3 panel for the shared dual-eye display chain");
    esp_err_t error = initialize_lcd_controller();
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "LCD initialization failed: %s", esp_err_to_name(error));
        release_lcd_resources();
        g_initialization_started.store(false, std::memory_order_release);
        return error;
    }

    g_frame_buffer = static_cast<std::uint16_t *>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (g_frame_buffer == nullptr) {
        ESP_LOGE(kTag, "cannot allocate %u-byte DMA frame", static_cast<unsigned>(kFrameBytes));
        release_lcd_resources();
        g_initialization_started.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    render_frame(g_state.load(std::memory_order_relaxed), 0);
    error = display_frame();
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "initial frame failed: %s", esp_err_to_name(error));
        heap_caps_free(g_frame_buffer);
        g_frame_buffer = nullptr;
        release_lcd_resources();
        g_initialization_started.store(false, std::memory_order_release);
        return error;
    }
    gpio_set_level(kLcdBacklightGpio, 1);

    if (xTaskCreate(
            animation_task,
            "eye_animation",
            4096,
            nullptr,
            tskIDLE_PRIORITY + 1,
            &g_animation_task) != pdPASS) {
        ESP_LOGE(kTag, "cannot create animation task");
        gpio_set_level(kLcdBacklightGpio, 0);
        heap_caps_free(g_frame_buffer);
        g_frame_buffer = nullptr;
        release_lcd_resources();
        g_initialization_started.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "shared eye display ready (one 51.2 KB DMA frame)");
    return ESP_OK;
}

void eye_display_set_state(EyeState state)
{
    g_state.store(state, std::memory_order_relaxed);
}

EyeState eye_display_state()
{
    return g_state.load(std::memory_order_relaxed);
}

}  // namespace companion
