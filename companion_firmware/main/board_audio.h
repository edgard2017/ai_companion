#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

class BoardAudio {
public:
    static constexpr uint32_t kSampleRate = 24000;

    esp_err_t Initialize();
    esp_err_t StartRecording();
    void StopRecording();
    esp_err_t Read(int16_t* samples, size_t sample_count);

    esp_err_t StartPlayback(int volume);
    void StopPlayback();
    esp_err_t Write(const int16_t* samples, size_t sample_count);

private:
    void CreateDuplexChannels();

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* output_ctrl_if_ = nullptr;
    const audio_codec_if_t* output_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* input_ctrl_if_ = nullptr;
    const audio_codec_if_t* input_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    esp_codec_dev_handle_t output_device_ = nullptr;
    esp_codec_dev_handle_t input_device_ = nullptr;

    bool recording_ = false;
    bool playing_ = false;
};
