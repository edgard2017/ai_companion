#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board_audio.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "repeater";
constexpr gpio_num_t kButton = GPIO_NUM_0;
constexpr size_t kWavHeaderSize = 44;
constexpr size_t kReadSamples = 1024;
constexpr size_t kMaxReplyBytes = 3 * 1024 * 1024;
constexpr uint32_t kMinimumRecordingMs = 300;
constexpr char kMultipartBoundary[] = "----esp32-s3-repeater-boundary";

BoardAudio board_audio;

struct HttpBuffer {
    uint8_t* data = nullptr;
    size_t size = 0;
};

struct WavView {
    const int16_t* samples = nullptr;
    size_t sample_count = 0;
    uint32_t sample_rate = 0;
};

uint16_t ReadLittleEndian16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t ReadLittleEndian32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 |
           static_cast<uint32_t>(bytes[3]) << 24;
}

void WriteLittleEndian16(uint8_t* bytes, uint16_t value)
{
    bytes[0] = value & 0xFF;
    bytes[1] = (value >> 8) & 0xFF;
}

void WriteLittleEndian32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = value & 0xFF;
    bytes[1] = (value >> 8) & 0xFF;
    bytes[2] = (value >> 16) & 0xFF;
    bytes[3] = (value >> 24) & 0xFF;
}

void WriteWavHeader(uint8_t* output, size_t pcm_bytes)
{
    std::memcpy(output, "RIFF", 4);
    WriteLittleEndian32(output + 4, static_cast<uint32_t>(36 + pcm_bytes));
    std::memcpy(output + 8, "WAVEfmt ", 8);
    WriteLittleEndian32(output + 16, 16);
    WriteLittleEndian16(output + 20, 1);
    WriteLittleEndian16(output + 22, 1);
    WriteLittleEndian32(output + 24, BoardAudio::kSampleRate);
    WriteLittleEndian32(output + 28, BoardAudio::kSampleRate * 2);
    WriteLittleEndian16(output + 32, 2);
    WriteLittleEndian16(output + 34, 16);
    std::memcpy(output + 36, "data", 4);
    WriteLittleEndian32(output + 40, static_cast<uint32_t>(pcm_bytes));
}

bool ParseWav(const uint8_t* data, size_t size, WavView* view)
{
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0) {
        ESP_LOGE(kTag, "Mac response is not a RIFF/WAVE file");
        return false;
    }

    bool format_found = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_size = 0;

    size_t offset = 12;
    while (offset + 8 <= size) {
        const uint8_t* chunk = data + offset;
        uint32_t chunk_size = ReadLittleEndian32(chunk + 4);
        size_t data_offset = offset + 8;
        if (chunk_size > size - data_offset) {
            ESP_LOGE(kTag, "Invalid WAV chunk length");
            return false;
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = ReadLittleEndian16(data + data_offset);
            channels = ReadLittleEndian16(data + data_offset + 2);
            view->sample_rate = ReadLittleEndian32(data + data_offset + 4);
            bits_per_sample = ReadLittleEndian16(data + data_offset + 14);
            format_found = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm = data + data_offset;
            pcm_size = chunk_size;
        }
        offset = data_offset + chunk_size + (chunk_size & 1U);
    }

    if (!format_found || pcm == nullptr || audio_format != 1 || channels != 1 ||
        bits_per_sample != 16 || view->sample_rate != BoardAudio::kSampleRate ||
        (pcm_size & 1U) != 0) {
        ESP_LOGE(kTag,
                 "Need 24kHz mono PCM16 WAV; got format=%u channels=%u rate=%lu bits=%u",
                 audio_format, channels, static_cast<unsigned long>(view->sample_rate),
                 bits_per_sample);
        return false;
    }

    view->samples = reinterpret_cast<const int16_t*>(pcm);
    view->sample_count = pcm_size / sizeof(int16_t);
    return true;
}

void InitializeButton()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << kButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
}

void WaitForButtonPress()
{
    // Do not retrigger if the user is still holding the button after an 8s recording.
    while (gpio_get_level(kButton) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (true) {
        while (gpio_get_level(kButton) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(kButton) == 0) {
            return;
        }
    }
}

void WaitForButtonRelease()
{
    while (gpio_get_level(kButton) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

size_t RecordWav(uint8_t* wav, size_t capacity)
{
    const size_t pcm_capacity = capacity - kWavHeaderSize;
    size_t pcm_size = 0;
    int32_t peak = 0;
    auto* pcm = reinterpret_cast<int16_t*>(wav + kWavHeaderSize);

    if (board_audio.StartRecording() != ESP_OK) {
        return 0;
    }
    ESP_LOGI(kTag, "Recording... release BOOT to send");

    // Let the codec and DMA settle, and discard the first short block.
    int16_t discard[kReadSamples];
    board_audio.Read(discard, kReadSamples);

    while (gpio_get_level(kButton) == 0 &&
           pcm_size + kReadSamples * sizeof(int16_t) <= pcm_capacity) {
        int16_t* destination = reinterpret_cast<int16_t*>(
            reinterpret_cast<uint8_t*>(pcm) + pcm_size);
        if (board_audio.Read(destination, kReadSamples) != ESP_OK) {
            break;
        }
        for (size_t index = 0; index < kReadSamples; ++index) {
            int32_t magnitude = destination[index];
            if (magnitude < 0) {
                magnitude = -magnitude;
            }
            peak = std::max(peak, magnitude);
        }
        pcm_size += kReadSamples * sizeof(int16_t);
    }
    board_audio.StopRecording();
    WaitForButtonRelease();

    WriteWavHeader(wav, pcm_size);
    uint32_t duration_ms = static_cast<uint32_t>(
        pcm_size * 1000ULL / (BoardAudio::kSampleRate * sizeof(int16_t)));
    ESP_LOGI(kTag, "Recorded %lu ms, peak=%ld, WAV=%u bytes",
             static_cast<unsigned long>(duration_ms), static_cast<long>(peak),
             static_cast<unsigned>(pcm_size + kWavHeaderSize));
    if (duration_ms < kMinimumRecordingMs) {
        ESP_LOGW(kTag, "Recording is too short; hold BOOT while speaking");
        return 0;
    }
    return pcm_size + kWavHeaderSize;
}

esp_err_t HttpWriteAll(
    esp_http_client_handle_t client, const uint8_t* data, size_t size)
{
    while (size > 0) {
        int written = esp_http_client_write(
            client, reinterpret_cast<const char*>(data), static_cast<int>(size));
        if (written <= 0) {
            ESP_LOGE(kTag, "HTTP upload write failed: %d", written);
            return ESP_FAIL;
        }
        data += written;
        size -= written;
    }
    return ESP_OK;
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
        std::strncmp(event->header_key, "X-", 2) == 0) {
        ESP_LOGI(kTag, "%s: %s", event->header_key, event->header_value);
    }
    return ESP_OK;
}

esp_err_t SendToMac(const uint8_t* wav, size_t wav_size, HttpBuffer* reply)
{
    char prefix[256];
    int prefix_size = std::snprintf(
        prefix, sizeof(prefix),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        kMultipartBoundary);
    char suffix[64];
    int suffix_size = std::snprintf(
        suffix, sizeof(suffix), "\r\n--%s--\r\n", kMultipartBoundary);
    if (prefix_size <= 0 || static_cast<size_t>(prefix_size) >= sizeof(prefix) ||
        suffix_size <= 0 || static_cast<size_t>(suffix_size) >= sizeof(suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {};
    config.url = wifi_manager::ServerUrl();
    config.event_handler = HttpEventHandler;
    config.timeout_ms = 180000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    char content_type[96];
    std::snprintf(content_type, sizeof(content_type),
                  "multipart/form-data; boundary=%s", kMultipartBoundary);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    int body_size = prefix_size + static_cast<int>(wav_size) + suffix_size;

    ESP_LOGI(kTag, "Uploading to %s", wifi_manager::ServerUrl());
    esp_err_t result = esp_http_client_open(client, body_size);
    if (result == ESP_OK) {
        result = HttpWriteAll(
            client, reinterpret_cast<const uint8_t*>(prefix), prefix_size);
    }
    if (result == ESP_OK) {
        result = HttpWriteAll(client, wav, wav_size);
    }
    if (result == ESP_OK) {
        result = HttpWriteAll(
            client, reinterpret_cast<const uint8_t*>(suffix), suffix_size);
    }

    int64_t content_length = -1;
    if (result == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(kTag, "Failed to receive HTTP response headers");
            result = ESP_FAIL;
        }
    }

    int status = esp_http_client_get_status_code(client);
    size_t capacity = content_length > 0
        ? static_cast<size_t>(content_length)
        : kMaxReplyBytes;
    if (result == ESP_OK && (capacity == 0 || capacity > kMaxReplyBytes)) {
        ESP_LOGE(kTag, "Mac response too large: %u bytes", static_cast<unsigned>(capacity));
        result = ESP_ERR_INVALID_SIZE;
    }

    if (result == ESP_OK) {
        reply->data = static_cast<uint8_t*>(
            heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (reply->data == nullptr) {
            result = ESP_ERR_NO_MEM;
        } else {
            int received = esp_http_client_read_response(
                client, reinterpret_cast<char*>(reply->data), static_cast<int>(capacity));
            if (received < 0) {
                result = ESP_FAIL;
            } else {
                reply->size = static_cast<size_t>(received);
                reply->data[reply->size] = 0;
            }
        }
    }

    if (result == ESP_OK && status != 200) {
        ESP_LOGE(kTag, "Mac returned HTTP %d: %.*s", status,
                 static_cast<int>(std::min(reply->size, size_t{300})), reply->data);
        result = ESP_FAIL;
    } else if (result == ESP_OK) {
        ESP_LOGI(kTag, "Received %u-byte TTS WAV", static_cast<unsigned>(reply->size));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        heap_caps_free(reply->data);
        reply->data = nullptr;
        reply->size = 0;
    }
    return result;
}

void PlayReply(const HttpBuffer& reply)
{
    WavView wav;
    if (!ParseWav(reply.data, reply.size, &wav)) {
        return;
    }
    ESP_LOGI(kTag, "Playing %.2f seconds", static_cast<double>(wav.sample_count) /
             BoardAudio::kSampleRate);
    if (board_audio.StartPlayback(CONFIG_REPEATER_SPEAKER_VOLUME) != ESP_OK) {
        return;
    }
    constexpr size_t kWriteSamples = 2048;
    for (size_t offset = 0; offset < wav.sample_count; offset += kWriteSamples) {
        size_t count = std::min(kWriteSamples, wav.sample_count - offset);
        if (board_audio.Write(wav.samples + offset, count) != ESP_OK) {
            break;
        }
    }
    board_audio.StopPlayback();
    ESP_LOGI(kTag, "Ready for the next sentence");
}

void InitializeNvs()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

}  // namespace

extern "C" void app_main()
{
    ESP_LOGI(kTag, "Zhengchen EYE repeater demo starting");
    ESP_LOGI(kTag, "Hold BOOT, speak, then release BOOT");

    InitializeNvs();
    InitializeButton();
    ESP_ERROR_CHECK(board_audio.Initialize());
    ESP_ERROR_CHECK(wifi_manager::Initialize());

    constexpr size_t kRecordingCapacity =
        kWavHeaderSize + CONFIG_REPEATER_RECORD_SECONDS *
        BoardAudio::kSampleRate * sizeof(int16_t);
    auto* recording = static_cast<uint8_t*>(
        heap_caps_malloc(kRecordingCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK(recording == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_LOGI(kTag, "PSRAM recording buffer: %u bytes",
             static_cast<unsigned>(kRecordingCapacity));

    while (true) {
        WaitForButtonPress();
        size_t recording_size = RecordWav(recording, kRecordingCapacity);
        if (recording_size == 0) {
            continue;
        }

        wifi_manager::WaitUntilConnected();
        HttpBuffer reply;
        if (SendToMac(recording, recording_size, &reply) == ESP_OK) {
            PlayReply(reply);
        } else {
            ESP_LOGE(kTag, "Repeat failed; check Mac server, firewall, and LAN connectivity");
        }
        heap_caps_free(reply.data);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
