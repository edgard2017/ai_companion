#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace companion {

// Audio response bodies are allocated in PSRAM. Call FreeAudioResponse after
// playback (or before reusing the same response object).
struct AudioResponse {
    uint8_t* data = nullptr;
    size_t size = 0;
    int http_status = 0;
};

void FreeAudioResponse(AudioResponse* response);

struct AsrResult {
    char text[384] = {};
    float audio_seconds = 0.0F;
    float processing_seconds = 0.0F;
    int http_status = 0;
};

struct DictationCreateRequest {
    const char* learner_id = "judy";
    const char* unit_id = nullptr;
    const char* mode = "chinese_word";
    // Set count to zero to let the server use every approved item.
    int count = 0;
    bool shuffle = false;
    // Used only when shuffle is true. nullptr lets the server choose a seed.
    const char* random_seed = nullptr;
};

struct DictationState {
    char session_id[129] = {};
    char mode[32] = {};
    char status[16] = {};
    char speech_text[512] = {};
    uint32_t revision = 0;
    uint32_t position = 0;
    uint32_t total_items = 0;
    bool done = false;
    int http_status = 0;
};

// Synchronous LAN client for AI Companion Server. It is intentionally not
// thread-safe: the firmware owns one instance from its central state machine.
class ServerClient {
public:
    ServerClient();

    // Accepts a base such as "http://mac-name.local:8765". A missing
    // scheme is treated as http://. Trailing slashes and a legacy /repeat,
    // /voice-chat, /asr or /tts endpoint are removed.
    esp_err_t Configure(const char* base_url, const char* bearer_token = nullptr);

    const char* BaseUrl() const;
    const char* LastError() const;
    int LastHttpStatus() const;

    // When LastHttpStatus() is 409, returns the server's current revision if
    // it was present in the error response, otherwise -1.
    int ConflictRevision() const;

    esp_err_t VoiceChat(
        const uint8_t* wav,
        size_t wav_size,
        const char* session_id,
        AudioResponse* response);

    esp_err_t Recognize(
        const uint8_t* wav,
        size_t wav_size,
        AsrResult* result,
        int timeout_ms);

    esp_err_t CreateDictation(
        const DictationCreateRequest& request,
        DictationState* state);

    esp_err_t DictationAction(
        const char* session_id,
        const char* action,
        const char* request_id,
        uint32_t expected_revision,
        DictationState* state);

    esp_err_t GetDictationState(
        const char* session_id,
        DictationState* state);

    esp_err_t GetDictationPrompt(
        const char* session_id,
        uint32_t revision,
        AudioResponse* response);

private:
    struct ResponseBody;
    struct BodyChunk;

    esp_err_t PerformRequest(
        const char* path,
        int method,
        const char* content_type,
        const char* accept,
        const BodyChunk* chunks,
        size_t chunk_count,
        const char* extra_header_name,
        const char* extra_header_value,
        int timeout_ms,
        size_t max_response_size,
        ResponseBody* response);

    esp_err_t PostWav(
        const char* path,
        const uint8_t* wav,
        size_t wav_size,
        const char* extra_header_name,
        const char* extra_header_value,
        int timeout_ms,
        size_t max_response_size,
        ResponseBody* response);

    esp_err_t PostJson(
        const char* path,
        const char* json,
        ResponseBody* response);

    esp_err_t ParseDictationState(
        const ResponseBody& response,
        DictationState* state);

    esp_err_t BuildUrl(const char* path, char* output, size_t output_size) const;
    void BeginRequest();
    esp_err_t HandleHttpStatus(const ResponseBody& response);
    void SetLastError(const char* format, ...);

    char base_url_[256];
    char authorization_[384];
    char last_error_[384];
    int last_http_status_;
    int conflict_revision_;
};

}  // namespace companion
