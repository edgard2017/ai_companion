#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "board_audio.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "eye_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "server_client.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "companion";
constexpr gpio_num_t kButton = GPIO_NUM_0;
constexpr size_t kWavHeaderSize = 44;
constexpr size_t kAudioFrameSamples = 512;
constexpr size_t kPlaybackFrameSamples = 2048;
constexpr uint32_t kMinimumChatMs = 300;
constexpr uint32_t kMinimumCommandMs = 180;
constexpr uint32_t kPreRollMs = CONFIG_COMPANION_VAD_PRE_ROLL_MS;
constexpr uint32_t kPlaybackEchoGuardMs = 250;
constexpr uint32_t kPlaybackDrainMs = 70;
constexpr int kNetworkAttempts = 3;

BoardAudio board_audio;
companion::ServerClient server_client;
using companion::EyeState;

struct EyeDisplayAdapter {
    esp_err_t Initialize() { return companion::eye_display_init(); }
    void SetState(EyeState state) { companion::eye_display_set_state(state); }
};

EyeDisplayAdapter eye_display;

portMUX_TYPE button_event_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool button_press_seen = false;
char conversation_session_id[64] = {};
uint32_t request_counter = 0;

struct WavView {
    const int16_t* samples = nullptr;
    size_t sample_count = 0;
    uint32_t sample_rate = 0;
};

enum class ChatCaptureResult {
    kShortPress,
    kRecording,
    kFailed,
};

enum class CommandCaptureResult {
    kSpeech,
    kButton,
    kTimeout,
    kFailed,
};

enum class DictationTrigger {
    kNext,
    kFinish,
    kFailed,
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
    if (data == nullptr || view == nullptr || size < 12 ||
        std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0) {
        ESP_LOGE(kTag, "Server response is not a RIFF/WAVE file");
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
        const uint32_t chunk_size = ReadLittleEndian32(chunk + 4);
        const size_t data_offset = offset + 8;
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
                 audio_format, channels,
                 static_cast<unsigned long>(view->sample_rate), bits_per_sample);
        return false;
    }

    view->samples = reinterpret_cast<const int16_t*>(pcm);
    view->sample_count = pcm_size / sizeof(int16_t);
    return true;
}

void IRAM_ATTR ButtonIsr(void*)
{
    portENTER_CRITICAL_ISR(&button_event_lock);
    button_press_seen = true;
    portEXIT_CRITICAL_ISR(&button_event_lock);
}

void ClearButtonEvent()
{
    portENTER_CRITICAL(&button_event_lock);
    button_press_seen = false;
    portEXIT_CRITICAL(&button_event_lock);
}

bool TakeButtonEvent()
{
    portENTER_CRITICAL(&button_event_lock);
    const bool seen = button_press_seen;
    button_press_seen = false;
    portEXIT_CRITICAL(&button_event_lock);
    return seen;
}

void InitializeButton()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << kButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_NEGEDGE;
    ESP_ERROR_CHECK(gpio_config(&config));
    esp_err_t result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(result);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(kButton, ButtonIsr, nullptr));
    ClearButtonEvent();
}

void WaitForButtonRelease()
{
    while (gpio_get_level(kButton) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ClearButtonEvent();
}

void WaitForButtonPress()
{
    if (gpio_get_level(kButton) == 0) {
        WaitForButtonRelease();
    }
    while (true) {
        ClearButtonEvent();
        while (gpio_get_level(kButton) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_COMPANION_BUTTON_DEBOUNCE_MS));
        if (gpio_get_level(kButton) == 0) {
            return;
        }
    }
}

int32_t MeanAbsoluteAmplitude(const int16_t* samples, size_t count)
{
    uint64_t total = 0;
    for (size_t index = 0; index < count; ++index) {
        int32_t value = samples[index];
        if (value < 0) {
            value = -value;
        }
        total += static_cast<uint32_t>(value);
    }
    return count == 0 ? 0 : static_cast<int32_t>(total / count);
}

ChatCaptureResult CaptureChatPress(uint8_t* wav, size_t capacity, size_t* wav_size)
{
    *wav_size = 0;
    const int64_t pressed_at = esp_timer_get_time();
    const size_t pcm_capacity = capacity - kWavHeaderSize;
    size_t pcm_size = 0;
    int32_t peak = 0;

    eye_display.SetState(EyeState::kListening);
    if (board_audio.StartRecording() != ESP_OK) {
        return ChatCaptureResult::kFailed;
    }

    int16_t frame[kAudioFrameSamples];
    board_audio.Read(frame, kAudioFrameSamples);
    while (gpio_get_level(kButton) == 0 &&
           pcm_size + sizeof(frame) <= pcm_capacity) {
        auto* destination = reinterpret_cast<int16_t*>(
            wav + kWavHeaderSize + pcm_size);
        if (board_audio.Read(destination, kAudioFrameSamples) != ESP_OK) {
            break;
        }
        for (size_t index = 0; index < kAudioFrameSamples; ++index) {
            int32_t value = destination[index];
            if (value < 0) {
                value = -value;
            }
            peak = std::max(peak, value);
        }
        pcm_size += sizeof(frame);
    }
    const int64_t released_at = esp_timer_get_time();
    board_audio.StopRecording();
    const uint32_t held_ms = static_cast<uint32_t>(
        (released_at - pressed_at) / 1000);
    WaitForButtonRelease();

    if (held_ms < CONFIG_COMPANION_BUTTON_CHAT_HOLD_MS) {
        ESP_LOGI(kTag, "Short press (%lu ms): start dictation",
                 static_cast<unsigned long>(held_ms));
        return ChatCaptureResult::kShortPress;
    }

    WriteWavHeader(wav, pcm_size);
    const uint32_t duration_ms = static_cast<uint32_t>(
        pcm_size * 1000ULL / (BoardAudio::kSampleRate * sizeof(int16_t)));
    ESP_LOGI(kTag, "Chat recording: %lu ms, mean peak=%ld, WAV=%u bytes",
             static_cast<unsigned long>(duration_ms), static_cast<long>(peak),
             static_cast<unsigned>(pcm_size + kWavHeaderSize));
    if (duration_ms < kMinimumChatMs) {
        return ChatCaptureResult::kFailed;
    }
    *wav_size = pcm_size + kWavHeaderSize;
    return ChatCaptureResult::kRecording;
}

CommandCaptureResult CaptureCommand(
    uint8_t* wav, size_t capacity, int64_t deadline_us, size_t* wav_size)
{
    *wav_size = 0;
    const size_t pcm_capacity = capacity - kWavHeaderSize;
    const size_t configured_pre_roll =
        (BoardAudio::kSampleRate * kPreRollMs / 1000 / kAudioFrameSamples) *
        kAudioFrameSamples * sizeof(int16_t);
    const size_t pre_roll_capacity =
        std::max(sizeof(int16_t) * kAudioFrameSamples, configured_pre_roll);
    size_t pcm_size = 0;
    bool speech_started = false;
    int64_t speech_started_at = 0;
    int64_t last_loud_at = 0;

    if (TakeButtonEvent() || gpio_get_level(kButton) == 0) {
        WaitForButtonRelease();
        return CommandCaptureResult::kButton;
    }
    if (esp_timer_get_time() >= deadline_us) {
        return CommandCaptureResult::kTimeout;
    }

    eye_display.SetState(EyeState::kDictation);
    if (board_audio.StartRecording() != ESP_OK) {
        return CommandCaptureResult::kFailed;
    }

    int16_t frame[kAudioFrameSamples];
    board_audio.Read(frame, kAudioFrameSamples);
    while (true) {
        if (TakeButtonEvent() || gpio_get_level(kButton) == 0) {
            board_audio.StopRecording();
            WaitForButtonRelease();
            return CommandCaptureResult::kButton;
        }

        const int64_t before_read = esp_timer_get_time();
        if (!speech_started && before_read >= deadline_us) {
            board_audio.StopRecording();
            return CommandCaptureResult::kTimeout;
        }
        if (board_audio.Read(frame, kAudioFrameSamples) != ESP_OK) {
            board_audio.StopRecording();
            return CommandCaptureResult::kFailed;
        }

        const int64_t now = esp_timer_get_time();
        const int32_t energy = MeanAbsoluteAmplitude(frame, kAudioFrameSamples);
        const bool loud = energy >= CONFIG_COMPANION_VAD_ENERGY_THRESHOLD;
        if (!speech_started) {
            if (pcm_size + sizeof(frame) > pre_roll_capacity) {
                const size_t shift = sizeof(frame);
                std::memmove(wav + kWavHeaderSize,
                             wav + kWavHeaderSize + shift,
                             pcm_size - shift);
                pcm_size -= shift;
            }
            std::memcpy(wav + kWavHeaderSize + pcm_size, frame, sizeof(frame));
            pcm_size += sizeof(frame);
            if (loud) {
                speech_started = true;
                speech_started_at = now;
                last_loud_at = now;
                eye_display.SetState(EyeState::kListening);
                ESP_LOGI(kTag, "Voice command detected, energy=%ld",
                         static_cast<long>(energy));
            }
            continue;
        }

        if (pcm_size + sizeof(frame) <= pcm_capacity) {
            std::memcpy(wav + kWavHeaderSize + pcm_size, frame, sizeof(frame));
            pcm_size += sizeof(frame);
        }
        if (loud) {
            last_loud_at = now;
        }

        const uint32_t speech_ms = static_cast<uint32_t>((now - speech_started_at) / 1000);
        const uint32_t silent_ms = static_cast<uint32_t>((now - last_loud_at) / 1000);
        const bool phrase_ended =
            speech_ms >= kMinimumCommandMs &&
            silent_ms >= CONFIG_COMPANION_VAD_END_SILENCE_MS;
        const bool phrase_full =
            speech_ms >= CONFIG_COMPANION_COMMAND_MAX_SECONDS * 1000U ||
            pcm_size + sizeof(frame) > pcm_capacity;
        if (phrase_ended || phrase_full) {
            board_audio.StopRecording();
            WriteWavHeader(wav, pcm_size);
            *wav_size = pcm_size + kWavHeaderSize;
            ESP_LOGI(kTag, "Command recording: %lu ms, WAV=%u bytes",
                     static_cast<unsigned long>(speech_ms),
                     static_cast<unsigned>(*wav_size));
            return CommandCaptureResult::kSpeech;
        }
    }
}

bool PlayAudio(const companion::AudioResponse& audio)
{
    WavView wav;
    if (!ParseWav(audio.data, audio.size, &wav)) {
        return false;
    }
    eye_display.SetState(EyeState::kSpeaking);
    ESP_LOGI(kTag, "Playing %.2f seconds",
             static_cast<double>(wav.sample_count) / BoardAudio::kSampleRate);
    if (board_audio.StartPlayback(CONFIG_COMPANION_SPEAKER_VOLUME) != ESP_OK) {
        return false;
    }
    bool ok = true;
    for (size_t offset = 0; offset < wav.sample_count; offset += kPlaybackFrameSamples) {
        const size_t count =
            std::min(kPlaybackFrameSamples, wav.sample_count - offset);
        if (board_audio.Write(wav.samples + offset, count) != ESP_OK) {
            ok = false;
            break;
        }
    }
    // esp_codec_dev_write may return once the final samples are queued in I2S
    // DMA. Let that tail leave the codec before closing it, otherwise the last
    // syllable can be clipped and the dictation timer can start too early.
    vTaskDelay(pdMS_TO_TICKS(kPlaybackDrainMs));
    board_audio.StopPlayback();
    return ok;
}

void ShowTransientError(const char* context)
{
    ESP_LOGE(kTag, "%s: %s (HTTP %d)", context, server_client.LastError(),
             server_client.LastHttpStatus());
    eye_display.SetState(EyeState::kError);
    vTaskDelay(pdMS_TO_TICKS(700));
}

void MakeRequestId(const char* action, char* output, size_t output_size)
{
    ++request_counter;
    std::snprintf(output, output_size, "%08lx-%s-%lu",
                  static_cast<unsigned long>(esp_random()), action,
                  static_cast<unsigned long>(request_counter));
}

bool ApplyDictationAction(companion::DictationState* state, const char* action)
{
    const companion::DictationState before = *state;
    char request_id[96];
    MakeRequestId(action, request_id, sizeof(request_id));
    uint32_t expected_revision = state->revision;

    for (int attempt = 1; attempt <= kNetworkAttempts; ++attempt) {
        eye_display.SetState(EyeState::kThinking);
        companion::DictationState updated = {};
        const esp_err_t result = server_client.DictationAction(
            state->session_id, action, request_id, expected_revision, &updated);
        if (result == ESP_OK) {
            *state = updated;
            ESP_LOGI(kTag, "Dictation %s -> revision=%lu position=%lu status=%s",
                     action, static_cast<unsigned long>(state->revision),
                     static_cast<unsigned long>(state->position), state->status);
            return true;
        }

        if (server_client.LastHttpStatus() == 409) {
            companion::DictationState current = {};
            if (server_client.GetDictationState(state->session_id, &current) == ESP_OK) {
                bool intended_transition_already_happened = false;
                bool safe_to_rebase = false;
                if (std::strcmp(action, "start") == 0) {
                    intended_transition_already_happened =
                        std::strcmp(current.status, "active") == 0 &&
                        current.position == 1;
                    safe_to_rebase =
                        std::strcmp(current.status, "ready") == 0 &&
                        current.position == 0;
                } else if (std::strcmp(action, "next") == 0) {
                    intended_transition_already_happened =
                        (!current.done && current.position == before.position + 1) ||
                        (current.done && before.position >= before.total_items);
                    // A concurrent repeat changes only revision. In that case it
                    // is safe to rebase this still-pending next action.
                    safe_to_rebase =
                        !current.done &&
                        std::strcmp(current.status, "active") == 0 &&
                        current.position == before.position;
                } else if (std::strcmp(action, "finish") == 0) {
                    intended_transition_already_happened = current.done;
                    safe_to_rebase =
                        !current.done && std::strcmp(current.status, "active") == 0;
                }
                *state = current;
                if (intended_transition_already_happened) {
                    ESP_LOGW(kTag,
                             "Action %s was already reflected at revision %lu",
                             action, static_cast<unsigned long>(current.revision));
                    return true;
                }
                if (safe_to_rebase && current.revision != expected_revision) {
                    expected_revision = current.revision;
                    // A 409 is a definite semantic conflict, not an uncertain
                    // transport retry. Use a fresh ID for the rebased action.
                    MakeRequestId(action, request_id, sizeof(request_id));
                    ESP_LOGW(kTag,
                             "Rebasing pending %s action onto revision %lu",
                             action, static_cast<unsigned long>(expected_revision));
                    continue;
                }
                ESP_LOGE(kTag,
                         "Unsafe dictation conflict: action=%s old_position=%lu "
                         "current_position=%lu status=%s",
                         action, static_cast<unsigned long>(before.position),
                         static_cast<unsigned long>(current.position), current.status);
                return false;
            }
        }

        ESP_LOGW(kTag, "Dictation %s attempt %d/%d failed: %s",
                 action, attempt, kNetworkAttempts, server_client.LastError());
        vTaskDelay(pdMS_TO_TICKS(250 * attempt));
    }
    return false;
}

bool DownloadAndPlayPrompt(companion::DictationState* state)
{
    for (int attempt = 1; attempt <= kNetworkAttempts; ++attempt) {
        eye_display.SetState(EyeState::kThinking);
        companion::AudioResponse prompt = {};
        const esp_err_t result = server_client.GetDictationPrompt(
            state->session_id, state->revision, &prompt);
        if (result == ESP_OK) {
            const bool played = PlayAudio(prompt);
            companion::FreeAudioResponse(&prompt);
            if (played) {
                return true;
            }
        } else if (server_client.LastHttpStatus() == 409) {
            companion::DictationState current = {};
            if (server_client.GetDictationState(state->session_id, &current) == ESP_OK) {
                *state = current;
            }
        }
        companion::FreeAudioResponse(&prompt);
        ESP_LOGW(kTag, "Prompt download attempt %d/%d failed: %s",
                 attempt, kNetworkAttempts, server_client.LastError());
        vTaskDelay(pdMS_TO_TICKS(300 * attempt));
    }
    return false;
}

bool ContainsAny(std::string_view text,
                 const std::initializer_list<std::string_view>& phrases)
{
    for (const std::string_view phrase : phrases) {
        if (text.find(phrase) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool IsNextCommand(std::string_view text)
{
    return ContainsAny(text, {"好了", "好啦", "下一个", "下一题", "继续"});
}

bool IsFinishCommand(std::string_view text)
{
    return ContainsAny(text, {"结束听写", "不听了", "退出听写"});
}

DictationTrigger WaitForDictationTrigger(
    uint8_t* command_wav, size_t command_capacity)
{
    ClearButtonEvent();
    vTaskDelay(pdMS_TO_TICKS(kPlaybackEchoGuardMs));
    const int64_t deadline_us = esp_timer_get_time() +
        static_cast<int64_t>(CONFIG_COMPANION_DICTATION_WAIT_SECONDS) * 1000000;
    ESP_LOGI(kTag,
             "Waiting %d seconds: say 好了/下一个, click BOOT, or let it time out",
             CONFIG_COMPANION_DICTATION_WAIT_SECONDS);

    while (true) {
        size_t wav_size = 0;
        const CommandCaptureResult capture = CaptureCommand(
            command_wav, command_capacity, deadline_us, &wav_size);
        if (capture == CommandCaptureResult::kButton ||
            capture == CommandCaptureResult::kTimeout) {
            const char* trigger_name = capture == CommandCaptureResult::kButton
                ? "BOOT button"
                : "timeout";
            ESP_LOGI(kTag, "Next trigger: %s", trigger_name);
            return DictationTrigger::kNext;
        }
        if (capture == CommandCaptureResult::kFailed) {
            return DictationTrigger::kFailed;
        }

        eye_display.SetState(EyeState::kThinking);
        companion::AsrResult asr = {};
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            ESP_LOGI(kTag, "Next trigger: timeout before command ASR");
            return DictationTrigger::kNext;
        }
        const int asr_timeout_ms = static_cast<int>(
            std::max<int64_t>(1, (remaining_us + 999) / 1000));
        const esp_err_t result = server_client.Recognize(
            command_wav, wav_size, &asr, asr_timeout_ms);

        // A physical click while ASR was in flight wins over the recognition result.
        if (TakeButtonEvent() || gpio_get_level(kButton) == 0) {
            if (gpio_get_level(kButton) == 0) {
                WaitForButtonRelease();
            }
            return DictationTrigger::kNext;
        }
        if (result == ESP_OK) {
            ESP_LOGI(kTag, "Dictation command ASR: '%s'", asr.text);
            const std::string_view text(asr.text);
            if (IsFinishCommand(text)) {
                return DictationTrigger::kFinish;
            }
            if (IsNextCommand(text)) {
                return DictationTrigger::kNext;
            }
        } else {
            ESP_LOGW(kTag, "Command ASR failed: %s", server_client.LastError());
        }

        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGI(kTag, "Next trigger: timeout after unrecognized speech");
            return DictationTrigger::kNext;
        }
        ESP_LOGI(kTag, "Not a next command; continuing the original 10-second window");
    }
}

bool RunDictation(uint8_t* command_wav, size_t command_capacity)
{
    wifi_manager::WaitUntilConnected();
    eye_display.SetState(EyeState::kThinking);

    companion::DictationCreateRequest request = {};
    request.learner_id = "judy";
    request.unit_id = CONFIG_COMPANION_DICTATION_UNIT_ID;
    request.mode = CONFIG_COMPANION_DICTATION_MODE;
    request.count = CONFIG_COMPANION_DICTATION_COUNT;
    request.shuffle = false;
    request.random_seed = nullptr;

    companion::DictationState state = {};
    if (server_client.CreateDictation(request, &state) != ESP_OK) {
        ShowTransientError("Create dictation failed");
        return false;
    }
    ESP_LOGI(kTag, "Created dictation session %s with %lu item(s)",
             state.session_id, static_cast<unsigned long>(state.total_items));

    if (!ApplyDictationAction(&state, "start") || !DownloadAndPlayPrompt(&state)) {
        ShowTransientError("Start dictation failed");
        return false;
    }

    while (!state.done) {
        const DictationTrigger trigger =
            WaitForDictationTrigger(command_wav, command_capacity);
        if (trigger == DictationTrigger::kFailed) {
            ShowTransientError("Listen for dictation command failed");
            return false;
        }

        const char* action =
            trigger == DictationTrigger::kFinish ? "finish" : "next";
        if (!ApplyDictationAction(&state, action)) {
            ShowTransientError("Advance dictation failed");
            return false;
        }

        // Once an action succeeds, only retry this revision's prompt. Never send
        // the next action again just because TTS download failed.
        if (!DownloadAndPlayPrompt(&state)) {
            ShowTransientError("Download dictation prompt failed");
            return false;
        }
        if (trigger == DictationTrigger::kFinish) {
            break;
        }
    }

    ESP_LOGI(kTag, "Dictation session finished; returning to chat mode");
    return true;
}

void RunVoiceChat(const uint8_t* wav, size_t wav_size)
{
    wifi_manager::WaitUntilConnected();
    eye_display.SetState(EyeState::kThinking);
    companion::AudioResponse reply = {};
    if (server_client.VoiceChat(
            wav, wav_size, conversation_session_id, &reply) == ESP_OK) {
        if (!PlayAudio(reply)) {
            ESP_LOGE(kTag, "Could not play the chat reply");
        }
    } else {
        ShowTransientError("Voice chat failed");
    }
    companion::FreeAudioResponse(&reply);
}

void InitializeNvs()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

}  // namespace

extern "C" void app_main()
{
    ESP_LOGI(kTag, "AI Companion chat + dictation firmware starting");
    ESP_LOGI(kTag, "Short BOOT click: dictation; hold BOOT and speak: chat");

    InitializeNvs();
    InitializeButton();
    ESP_ERROR_CHECK(eye_display.Initialize());
    eye_display.SetState(EyeState::kThinking);
    ESP_ERROR_CHECK(board_audio.Initialize());
    ESP_ERROR_CHECK(wifi_manager::Initialize());
    ESP_ERROR_CHECK(server_client.Configure(
        wifi_manager::ServerBaseUrl(), CONFIG_COMPANION_API_TOKEN));

    std::snprintf(conversation_session_id, sizeof(conversation_session_id),
                  "esp32-%08lx-%08lx", static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));

    constexpr size_t kChatCapacity = kWavHeaderSize +
        CONFIG_COMPANION_CHAT_RECORD_SECONDS * BoardAudio::kSampleRate *
        sizeof(int16_t);
    constexpr size_t kCommandCapacity = kWavHeaderSize +
        CONFIG_COMPANION_COMMAND_MAX_SECONDS * BoardAudio::kSampleRate *
        sizeof(int16_t);
    auto* chat_wav = static_cast<uint8_t*>(
        heap_caps_malloc(kChatCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* command_wav = static_cast<uint8_t*>(
        heap_caps_malloc(kCommandCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK(chat_wav == nullptr || command_wav == nullptr
                        ? ESP_ERR_NO_MEM
                        : ESP_OK);
    ESP_LOGI(kTag, "Mac server: %s", server_client.BaseUrl());
    ESP_LOGI(kTag, "PSRAM buffers: chat=%u command=%u bytes",
             static_cast<unsigned>(kChatCapacity),
             static_cast<unsigned>(kCommandCapacity));

    while (true) {
        eye_display.SetState(EyeState::kIdle);
        ESP_LOGI(kTag,
                 "Ready: short BOOT click starts dictation; hold BOOT while speaking chats");
        WaitForButtonPress();

        size_t wav_size = 0;
        const ChatCaptureResult capture =
            CaptureChatPress(chat_wav, kChatCapacity, &wav_size);
        if (capture == ChatCaptureResult::kShortPress) {
            RunDictation(command_wav, kCommandCapacity);
        } else if (capture == ChatCaptureResult::kRecording) {
            RunVoiceChat(chat_wav, wav_size);
        } else {
            ESP_LOGW(kTag, "Recording failed or was too short");
            eye_display.SetState(EyeState::kError);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
