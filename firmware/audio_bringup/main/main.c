// audio_bringup —— 陪读小精灵 音频硬件自检固件（不涉及 WiFi/云端）
//
// 目的：在写任何网络代码之前，先证明「麦克风 + 功放 + I2S 收发」硬件是通的。
// 流程：
//   1) 开机播 2 秒 440Hz 正弦音   -> 验证 功放(MAX98357A)+喇叭
//   2) 进入麦克风->喇叭 loopback   -> 对着 INMP441 说话，喇叭能听到
//
// 关键格式记忆点：
//   - INMP441 是 I2S 数字麦：24bit 数据塞在 32bit 帧里、MSB 对齐、L/R 接地=左声道
//     => RX 用 32bit 读，取样后右移得到有效值。配错这里就是「噪声/静音」。
//   - MAX98357A 是 I2S 数字 D 类功放：直接吃 I2S，无需 DAC（S3 本来也没 DAC）。
//   - 两路用不同 I2S 端口：TX=I2S_NUM_0(功放)，RX=I2S_NUM_1(麦)，互不干扰。

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "bsp_pins.h"

static const char *TAG = "audio_bringup";

#define SAMPLE_RATE   16000     // 16k 足够语音；ASR/TTS 也常用 16k
#define TONE_HZ       440       // A4 标准音
#define TONE_SECONDS  2
#define MIC_GAIN_SHIFT 11       // INMP441 32bit->16bit 的增益移位，太吵就调大，太小声调小

static i2s_chan_handle_t s_tx_amp = NULL;   // 到功放
static i2s_chan_handle_t s_rx_mic = NULL;   // 从麦克风

// ---- 初始化功放 TX (I2S_NUM_0) ----
static void amp_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_amp, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_AMP_BCK,
            .ws   = I2S_AMP_WS,
            .dout = I2S_AMP_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_amp, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_amp));
    ESP_LOGI(TAG, "功放 I2S_NUM_0 就绪 (BCK=%d WS=%d DOUT=%d)",
             I2S_AMP_BCK, I2S_AMP_WS, I2S_AMP_DOUT);
}

// ---- 初始化麦克风 RX (I2S_NUM_1) ----
static void mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_mic));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // INMP441：32bit 帧，单声道
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_BCK,
            .ws   = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_MIC_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // INMP441 的 L/R 脚接 GND -> 数据在左声道
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_mic, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_mic));
    ESP_LOGI(TAG, "麦克风 I2S_NUM_1 就绪 (BCK=%d WS=%d DIN=%d)",
             I2S_MIC_BCK, I2S_MIC_WS, I2S_MIC_DIN);
}

// ---- 播放一段正弦音，验证功放+喇叭 ----
static void play_test_tone(void)
{
    const int period = SAMPLE_RATE / TONE_HZ;   // 一个周期的采样数（约）
    int16_t buf[256];
    for (int i = 0; i < 256; i++) {
        float ph = 2.0f * (float)M_PI * (float)(i % period) / (float)period;
        buf[i] = (int16_t)(sinf(ph) * 8000.0f);   // ~1/4 满幅，别太吵
    }

    size_t written = 0;
    const int total_bytes = SAMPLE_RATE * TONE_SECONDS * (int)sizeof(int16_t);
    int sent = 0;
    ESP_LOGI(TAG, "播放 %dHz 测试音 %d 秒 ...", TONE_HZ, TONE_SECONDS);
    while (sent < total_bytes) {
        i2s_channel_write(s_tx_amp, buf, sizeof(buf), &written, portMAX_DELAY);
        sent += (int)written;
    }
    ESP_LOGI(TAG, "测试音结束");
}

// ---- 麦克风 -> 功放 loopback，验证麦克风 ----
static void loopback_loop(void)
{
    static int32_t rx[256];      // 麦克风 32bit 原始
    static int16_t tx[256];      // 转成 16bit 喂功放
    size_t got = 0, put = 0;

    ESP_LOGI(TAG, "进入 loopback：对着麦克风说话，喇叭应能听到");
    while (1) {
        if (i2s_channel_read(s_rx_mic, rx, sizeof(rx), &got, portMAX_DELAY) != ESP_OK)
            continue;
        int n = (int)(got / sizeof(int32_t));
        for (int i = 0; i < n; i++) {
            // INMP441 有效数据在高位，右移取 16bit 并加增益
            int32_t s = rx[i] >> MIC_GAIN_SHIFT;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            tx[i] = (int16_t)s;
        }
        i2s_channel_write(s_tx_amp, tx, n * sizeof(int16_t), &put, portMAX_DELAY);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==== audio_bringup 启动 ====");
    amp_init();
    mic_init();
    play_test_tone();      // 第一步：验证功放+喇叭
    loopback_loop();       // 第二步：验证麦克风（不返回）
}
