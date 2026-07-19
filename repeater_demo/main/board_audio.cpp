#include "board_audio.h"

#include <cassert>

#include "driver/gpio.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr gpio_num_t kI2cSda = GPIO_NUM_1;
constexpr gpio_num_t kI2cScl = GPIO_NUM_2;
constexpr gpio_num_t kMclk = GPIO_NUM_38;
constexpr gpio_num_t kWordSelect = GPIO_NUM_13;
constexpr gpio_num_t kBitClock = GPIO_NUM_14;
constexpr gpio_num_t kDataIn = GPIO_NUM_12;
constexpr gpio_num_t kDataOut = GPIO_NUM_45;

constexpr uint8_t kEs8311Address = ES8311_CODEC_DEFAULT_ADDR;
// This board straps ES7210 AD0 high, so its 8-bit I2C address is 0x82.
constexpr uint8_t kEs7210Address = 0x82;
constexpr float kMicrophoneGainDb = 30.0F;
constexpr char kTag[] = "board_audio";

esp_err_t CodecResult(int result, const char* operation)
{
    if (result == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(kTag, "%s failed: codec error %d", operation, result);
    return ESP_FAIL;
}

}  // namespace

void BoardAudio::CreateDuplexChannels()
{
    i2s_chan_config_t channel_config = {};
    channel_config.id = I2S_NUM_0;
    channel_config.role = I2S_ROLE_MASTER;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 240;
    channel_config.auto_clear_after_cb = true;
    channel_config.auto_clear_before_cb = false;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &tx_handle_, &rx_handle_));

    i2s_std_config_t output_config = {};
    output_config.clk_cfg.sample_rate_hz = kSampleRate;
    output_config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    output_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    output_config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    output_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    output_config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    output_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    output_config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    output_config.slot_cfg.bit_shift = true;
    output_config.slot_cfg.left_align = true;
    output_config.gpio_cfg.mclk = kMclk;
    output_config.gpio_cfg.bclk = kBitClock;
    output_config.gpio_cfg.ws = kWordSelect;
    output_config.gpio_cfg.dout = kDataOut;
    output_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &output_config));

    i2s_tdm_config_t input_config = {};
    input_config.clk_cfg.sample_rate_hz = kSampleRate;
    input_config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    input_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    input_config.clk_cfg.bclk_div = 8;
    input_config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    input_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    input_config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    input_config.slot_cfg.slot_mask = static_cast<i2s_tdm_slot_mask_t>(
        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
    input_config.slot_cfg.ws_width = I2S_TDM_AUTO_WS_WIDTH;
    input_config.slot_cfg.bit_shift = true;
    input_config.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
    input_config.gpio_cfg.mclk = kMclk;
    input_config.gpio_cfg.bclk = kBitClock;
    input_config.gpio_cfg.ws = kWordSelect;
    input_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    input_config.gpio_cfg.din = kDataIn;
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &input_config));
}

esp_err_t BoardAudio::Initialize()
{
    i2c_master_bus_config_t i2c_config = {};
    i2c_config.i2c_port = I2C_NUM_0;
    i2c_config.sda_io_num = kI2cSda;
    i2c_config.scl_io_num = kI2cScl;
    i2c_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_config.glitch_ignore_cnt = 7;
    i2c_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &i2c_bus_), kTag, "I2C init failed");

    CreateDuplexChannels();

    audio_codec_i2s_cfg_t i2s_config = {};
    i2s_config.port = I2S_NUM_0;
    i2s_config.rx_handle = rx_handle_;
    i2s_config.tx_handle = tx_handle_;
    data_if_ = audio_codec_new_i2s_data(&i2s_config);
    assert(data_if_ != nullptr);

    audio_codec_i2c_cfg_t i2c_codec_config = {};
    i2c_codec_config.port = I2C_NUM_0;
    i2c_codec_config.addr = kEs8311Address;
    i2c_codec_config.bus_handle = i2c_bus_;
    output_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_codec_config);
    assert(output_ctrl_if_ != nullptr);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != nullptr);

    es8311_codec_cfg_t es8311_config = {};
    es8311_config.ctrl_if = output_ctrl_if_;
    es8311_config.gpio_if = gpio_if_;
    es8311_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_config.pa_pin = GPIO_NUM_NC;
    es8311_config.use_mclk = true;
    es8311_config.hw_gain.pa_voltage = 5.0F;
    es8311_config.hw_gain.codec_dac_voltage = 3.3F;
    output_codec_if_ = es8311_codec_new(&es8311_config);
    assert(output_codec_if_ != nullptr);

    esp_codec_dev_cfg_t output_device_config = {};
    output_device_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    output_device_config.codec_if = output_codec_if_;
    output_device_config.data_if = data_if_;
    output_device_ = esp_codec_dev_new(&output_device_config);
    assert(output_device_ != nullptr);

    i2c_codec_config.addr = kEs7210Address;
    input_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_codec_config);
    assert(input_ctrl_if_ != nullptr);

    es7210_codec_cfg_t es7210_config = {};
    es7210_config.ctrl_if = input_ctrl_if_;
    es7210_config.mic_selected =
        ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4;
    input_codec_if_ = es7210_codec_new(&es7210_config);
    assert(input_codec_if_ != nullptr);

    esp_codec_dev_cfg_t input_device_config = {};
    input_device_config.dev_type = ESP_CODEC_DEV_TYPE_IN;
    input_device_config.codec_if = input_codec_if_;
    input_device_config.data_if = data_if_;
    input_device_ = esp_codec_dev_new(&input_device_config);
    assert(input_device_ != nullptr);

    ESP_LOGI(kTag, "ES7210 + ES8311 initialized at %lu Hz",
             static_cast<unsigned long>(kSampleRate));
    return ESP_OK;
}

esp_err_t BoardAudio::StartRecording()
{
    StopPlayback();
    if (recording_) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t format = {};
    format.bits_per_sample = 16;
    format.channel = 4;
    format.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    format.sample_rate = kSampleRate;
    esp_err_t result = CodecResult(
        esp_codec_dev_open(input_device_, &format), "open microphone");
    if (result != ESP_OK) {
        return result;
    }
    result = CodecResult(
        esp_codec_dev_set_in_channel_gain(
            input_device_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), kMicrophoneGainDb),
        "set microphone gain");
    if (result != ESP_OK) {
        esp_codec_dev_close(input_device_);
        return result;
    }
    recording_ = true;
    return ESP_OK;
}

void BoardAudio::StopRecording()
{
    if (recording_) {
        esp_codec_dev_close(input_device_);
        recording_ = false;
    }
}

esp_err_t BoardAudio::Read(int16_t* samples, size_t sample_count)
{
    if (!recording_) {
        return ESP_ERR_INVALID_STATE;
    }
    return CodecResult(
        esp_codec_dev_read(input_device_, samples, sample_count * sizeof(int16_t)),
        "read microphone");
}

esp_err_t BoardAudio::StartPlayback(int volume)
{
    StopRecording();
    if (playing_) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t format = {};
    format.bits_per_sample = 16;
    format.channel = 1;
    format.sample_rate = kSampleRate;
    esp_err_t result = CodecResult(
        esp_codec_dev_open(output_device_, &format), "open speaker");
    if (result != ESP_OK) {
        return result;
    }
    result = CodecResult(
        esp_codec_dev_set_out_vol(output_device_, volume), "set speaker volume");
    if (result != ESP_OK) {
        esp_codec_dev_close(output_device_);
        return result;
    }
    playing_ = true;
    return ESP_OK;
}

void BoardAudio::StopPlayback()
{
    if (playing_) {
        esp_codec_dev_close(output_device_);
        playing_ = false;
    }
}

esp_err_t BoardAudio::Write(const int16_t* samples, size_t sample_count)
{
    if (!playing_) {
        return ESP_ERR_INVALID_STATE;
    }
    return CodecResult(
        esp_codec_dev_write(
            output_device_, const_cast<int16_t*>(samples), sample_count * sizeof(int16_t)),
        "write speaker");
}
