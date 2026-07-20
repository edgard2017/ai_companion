#include "server_client.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace companion {
namespace {

constexpr char kTag[] = "server_client";
constexpr char kMultipartBoundary[] =
    "----ai-companion-esp32-boundary-7d3a9b";
constexpr size_t kMaximumUploadBytes = 10 * 1024 * 1024;
constexpr size_t kMaximumAudioResponseBytes = 3 * 1024 * 1024;
constexpr size_t kMaximumJsonResponseBytes = 16 * 1024;
constexpr size_t kInitialResponseCapacity = 4096;
constexpr int kJsonTimeoutMs = 30000;
constexpr int kAudioTimeoutMs = 180000;

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

bool Validate24kMonoPcm16Wav(const uint8_t* wav, size_t wav_size)
{
    if (wav == nullptr || wav_size < 44 || wav_size > kMaximumUploadBytes ||
        std::memcmp(wav, "RIFF", 4) != 0 ||
        std::memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool valid_format = false;
    bool has_audio = false;
    size_t offset = 12;
    while (offset + 8 <= wav_size) {
        const uint8_t* chunk = wav + offset;
        const uint32_t chunk_size = ReadLittleEndian32(chunk + 4);
        const size_t data_offset = offset + 8;
        if (chunk_size > wav_size - data_offset) {
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            const uint8_t* format = wav + data_offset;
            valid_format = ReadLittleEndian16(format) == 1 &&
                           ReadLittleEndian16(format + 2) == 1 &&
                           ReadLittleEndian32(format + 4) == 24000 &&
                           ReadLittleEndian16(format + 14) == 16;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            has_audio = chunk_size > 0 && (chunk_size & 1U) == 0;
        }

        const size_t padded_size = static_cast<size_t>(chunk_size) +
                                   (chunk_size & 1U);
        if (padded_size > wav_size - data_offset) {
            return false;
        }
        offset = data_offset + padded_size;
    }
    return valid_format && has_audio;
}

bool EndsWith(const char* value, const char* suffix)
{
    const size_t value_size = std::strlen(value);
    const size_t suffix_size = std::strlen(suffix);
    return value_size >= suffix_size &&
           std::memcmp(value + value_size - suffix_size, suffix, suffix_size) == 0;
}

bool ContainsHeaderBreak(const char* value)
{
    return value != nullptr &&
           (std::strchr(value, '\r') != nullptr || std::strchr(value, '\n') != nullptr);
}

bool IsSafePathToken(const char* value)
{
    if (value == nullptr) {
        return false;
    }
    const size_t length = std::strlen(value);
    if (length == 0 || length > 128) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(character) || character == '-' || character == '_' ||
              character == '.' || character == '~')) {
            return false;
        }
    }
    return true;
}

bool IsAllowedAction(const char* action)
{
    return action != nullptr &&
           (std::strcmp(action, "start") == 0 ||
            std::strcmp(action, "next") == 0 ||
            std::strcmp(action, "repeat") == 0 ||
            std::strcmp(action, "finish") == 0);
}

bool IsAllowedMode(const char* mode)
{
    return mode != nullptr &&
           (std::strcmp(mode, "chinese_word") == 0 ||
            std::strcmp(mode, "english_spelling") == 0 ||
            std::strcmp(mode, "english_meaning") == 0);
}

bool CopyText(char* destination, size_t capacity, const char* source)
{
    if (destination == nullptr || capacity == 0 || source == nullptr) {
        return false;
    }
    const size_t source_size = std::strlen(source);
    if (source_size >= capacity) {
        return false;
    }
    std::memcpy(destination, source, source_size + 1);
    return true;
}

bool CopyTextTruncated(char* destination, size_t capacity, const char* source)
{
    if (destination == nullptr || capacity == 0 || source == nullptr) {
        return false;
    }
    size_t copy_size = std::min(std::strlen(source), capacity - 1);
    // If truncation lands inside a UTF-8 continuation sequence, exclude that
    // partial code point so logs and future UI use still receive valid text.
    while (copy_size > 0 &&
           (static_cast<unsigned char>(source[copy_size]) & 0xC0U) == 0x80U) {
        --copy_size;
    }
    std::memcpy(destination, source, copy_size);
    destination[copy_size] = '\0';
    return true;
}

bool JsonUnsigned(const cJSON* value, uint32_t* output)
{
    if (!cJSON_IsNumber(value) || output == nullptr || value->valuedouble < 0.0 ||
        value->valuedouble > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    const uint32_t converted = static_cast<uint32_t>(value->valuedouble);
    if (static_cast<double>(converted) != value->valuedouble) {
        return false;
    }
    *output = converted;
    return true;
}

esp_err_t HttpWriteAll(
    esp_http_client_handle_t client,
    const uint8_t* data,
    size_t size)
{
    while (size > 0) {
        const size_t next_size = std::min(
            size, static_cast<size_t>(std::numeric_limits<int>::max()));
        const int written = esp_http_client_write(
            client, reinterpret_cast<const char*>(data),
            static_cast<int>(next_size));
        if (written <= 0) {
            return ESP_FAIL;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return ESP_OK;
}

}  // namespace

struct ServerClient::BodyChunk {
    const uint8_t* data;
    size_t size;
};

struct ServerClient::ResponseBody {
    uint8_t* data = nullptr;
    size_t size = 0;
    int http_status = 0;
};

void FreeAudioResponse(AudioResponse* response)
{
    if (response == nullptr) {
        return;
    }
    heap_caps_free(response->data);
    response->data = nullptr;
    response->size = 0;
    response->http_status = 0;
}

ServerClient::ServerClient()
    : base_url_{},
      authorization_{},
      last_error_{},
      last_http_status_(0),
      conflict_revision_(-1)
{
}

esp_err_t ServerClient::Configure(const char* base_url, const char* bearer_token)
{
    BeginRequest();
    base_url_[0] = '\0';
    authorization_[0] = '\0';
    if (base_url == nullptr) {
        SetLastError("server base URL is missing");
        return ESP_ERR_INVALID_ARG;
    }

    const char* begin = base_url;
    while (*begin != '\0' && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    const char* end = begin + std::strlen(begin);
    while (end > begin && std::isspace(static_cast<unsigned char>(end[-1]))) {
        --end;
    }
    if (begin == end) {
        SetLastError("server base URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[sizeof(base_url_)] = {};
    const bool has_scheme =
        static_cast<size_t>(end - begin) >= 7 &&
        (std::memcmp(begin, "http://", 7) == 0 ||
         (static_cast<size_t>(end - begin) >= 8 &&
          std::memcmp(begin, "https://", 8) == 0));
    if (!has_scheme && std::strstr(begin, "://") != nullptr) {
        SetLastError("server base URL scheme must be http or https");
        return ESP_ERR_INVALID_ARG;
    }
    int normalized_size = 0;
    if (has_scheme) {
        normalized_size = std::snprintf(
            normalized, sizeof(normalized), "%.*s",
            static_cast<int>(end - begin), begin);
    } else {
        normalized_size = std::snprintf(
            normalized, sizeof(normalized), "http://%.*s",
            static_cast<int>(end - begin), begin);
    }
    if (normalized_size <= 0 ||
        static_cast<size_t>(normalized_size) >= sizeof(normalized)) {
        SetLastError("server base URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }
    if (std::strchr(normalized, '?') != nullptr ||
        std::strchr(normalized, '#') != nullptr ||
        std::strchr(normalized, ' ') != nullptr) {
        SetLastError("server base URL contains an unsupported query, fragment, or space");
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = std::strlen(normalized);
    while (length > 0 && normalized[length - 1] == '/') {
        normalized[--length] = '\0';
    }
    constexpr const char* kLegacyEndpoints[] = {
        "/voice-chat", "/repeat", "/asr", "/tts",
    };
    for (const char* endpoint : kLegacyEndpoints) {
        if (EndsWith(normalized, endpoint)) {
            normalized[std::strlen(normalized) - std::strlen(endpoint)] = '\0';
            break;
        }
    }
    length = std::strlen(normalized);
    while (length > 0 && normalized[length - 1] == '/') {
        normalized[--length] = '\0';
    }

    const char* authority = std::strstr(normalized, "://");
    if (authority == nullptr || authority[3] == '\0') {
        SetLastError("server base URL has no host");
        return ESP_ERR_INVALID_ARG;
    }
    CopyText(base_url_, sizeof(base_url_), normalized);

    if (bearer_token != nullptr && bearer_token[0] != '\0') {
        if (ContainsHeaderBreak(bearer_token)) {
            base_url_[0] = '\0';
            SetLastError("bearer token contains an invalid line break");
            return ESP_ERR_INVALID_ARG;
        }
        const bool already_prefixed = std::strncmp(bearer_token, "Bearer ", 7) == 0;
        const int authorization_size = already_prefixed
            ? std::snprintf(authorization_, sizeof(authorization_), "%s", bearer_token)
            : std::snprintf(
                  authorization_, sizeof(authorization_), "Bearer %s", bearer_token);
        if (authorization_size <= 0 ||
            static_cast<size_t>(authorization_size) >= sizeof(authorization_)) {
            base_url_[0] = '\0';
            authorization_[0] = '\0';
            SetLastError("bearer token is too long");
            return ESP_ERR_INVALID_SIZE;
        }
    }
    ESP_LOGI(kTag, "Server base URL: %s", base_url_);
    return ESP_OK;
}

const char* ServerClient::BaseUrl() const
{
    return base_url_;
}

const char* ServerClient::LastError() const
{
    return last_error_;
}

int ServerClient::LastHttpStatus() const
{
    return last_http_status_;
}

int ServerClient::ConflictRevision() const
{
    return conflict_revision_;
}

esp_err_t ServerClient::BuildUrl(
    const char* path, char* output, size_t output_size) const
{
    if (base_url_[0] == '\0' || path == nullptr || path[0] != '/' ||
        output == nullptr || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const int size = std::snprintf(output, output_size, "%s%s", base_url_, path);
    if (size <= 0 || static_cast<size_t>(size) >= output_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

void ServerClient::BeginRequest()
{
    last_error_[0] = '\0';
    last_http_status_ = 0;
    conflict_revision_ = -1;
}

void ServerClient::SetLastError(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(last_error_, sizeof(last_error_), format, arguments);
    va_end(arguments);
}

esp_err_t ServerClient::PerformRequest(
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
    ResponseBody* response)
{
    if (response == nullptr || max_response_size == 0 ||
        (chunk_count > 0 && chunks == nullptr) ||
        (extra_header_name == nullptr) != (extra_header_value == nullptr) ||
        ContainsHeaderBreak(extra_header_name) || ContainsHeaderBreak(extra_header_value)) {
        SetLastError("invalid HTTP request arguments");
        return ESP_ERR_INVALID_ARG;
    }
    response->data = nullptr;
    response->size = 0;
    response->http_status = 0;

    char url[512];
    esp_err_t result = BuildUrl(path, url, sizeof(url));
    if (result != ESP_OK) {
        SetLastError("could not build request URL");
        return result;
    }

    size_t upload_size = 0;
    for (size_t index = 0; index < chunk_count; ++index) {
        if ((chunks[index].data == nullptr && chunks[index].size != 0) ||
            chunks[index].size > static_cast<size_t>(std::numeric_limits<int>::max()) -
                                     upload_size) {
            SetLastError("HTTP upload body is too large");
            return ESP_ERR_INVALID_SIZE;
        }
        upload_size += chunks[index].size;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = timeout_ms;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        SetLastError("could not allocate HTTP client");
        return ESP_ERR_NO_MEM;
    }

    bool opened = false;
    esp_http_client_set_method(client, static_cast<esp_http_client_method_t>(method));
    if (content_type != nullptr) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (accept != nullptr) {
        esp_http_client_set_header(client, "Accept", accept);
    }
    if (authorization_[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", authorization_);
    }
    if (extra_header_name != nullptr) {
        esp_http_client_set_header(client, extra_header_name, extra_header_value);
    }

    ESP_LOGI(kTag, "%s %s", method == HTTP_METHOD_GET ? "GET" : "POST", url);
    result = esp_http_client_open(client, static_cast<int>(upload_size));
    if (result == ESP_OK) {
        opened = true;
        for (size_t index = 0; index < chunk_count && result == ESP_OK; ++index) {
            result = HttpWriteAll(client, chunks[index].data, chunks[index].size);
        }
    }
    if (result != ESP_OK) {
        SetLastError("HTTP upload/connect failed: %s", esp_err_to_name(result));
    }

    int64_t content_length = 0;
    if (result == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            result = content_length == -ESP_ERR_HTTP_EAGAIN
                ? ESP_ERR_TIMEOUT
                : ESP_FAIL;
            SetLastError("HTTP response headers failed: %s", esp_err_to_name(result));
        } else {
            response->http_status = esp_http_client_get_status_code(client);
            last_http_status_ = response->http_status;
        }
    }

    size_t capacity = 0;
    if (result == ESP_OK) {
        if (content_length > static_cast<int64_t>(max_response_size)) {
            SetLastError(
                "HTTP response is too large (%lld > %u bytes)",
                static_cast<long long>(content_length),
                static_cast<unsigned>(max_response_size));
            result = ESP_ERR_INVALID_SIZE;
        } else {
            capacity = content_length > 0
                ? static_cast<size_t>(content_length)
                : std::min(kInitialResponseCapacity, max_response_size);
            response->data = static_cast<uint8_t*>(heap_caps_malloc(
                capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (response->data == nullptr) {
                SetLastError("not enough PSRAM for HTTP response");
                result = ESP_ERR_NO_MEM;
            }
        }
    }

    while (result == ESP_OK && !esp_http_client_is_complete_data_received(client)) {
        if (response->size == capacity) {
            if (capacity >= max_response_size) {
                SetLastError("HTTP response exceeds %u bytes",
                             static_cast<unsigned>(max_response_size));
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            const size_t next_capacity = std::min(
                max_response_size,
                std::max(capacity * 2, capacity + kInitialResponseCapacity));
            void* grown = heap_caps_realloc(
                response->data, next_capacity + 1,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (grown == nullptr) {
                SetLastError("not enough PSRAM to grow HTTP response");
                result = ESP_ERR_NO_MEM;
                break;
            }
            response->data = static_cast<uint8_t*>(grown);
            capacity = next_capacity;
        }

        const size_t available = capacity - response->size;
        const int received = esp_http_client_read(
            client,
            reinterpret_cast<char*>(response->data + response->size),
            static_cast<int>(available));
        if (received < 0) {
            SetLastError("HTTP response read failed");
            result = ESP_FAIL;
        } else if (received == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                SetLastError("HTTP response ended before it was complete");
                result = ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            response->size += static_cast<size_t>(received);
        }
    }

    if (result == ESP_OK) {
        response->data[response->size] = 0;
        result = HandleHttpStatus(*response);
    }

    if (opened) {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        heap_caps_free(response->data);
        response->data = nullptr;
        response->size = 0;
    }
    return result;
}

esp_err_t ServerClient::HandleHttpStatus(const ResponseBody& response)
{
    if (response.http_status >= 200 && response.http_status < 300) {
        return ESP_OK;
    }

    const cJSON* detail = nullptr;
    cJSON* root = response.data == nullptr
        ? nullptr
        : cJSON_ParseWithLength(
              reinterpret_cast<const char*>(response.data), response.size);
    if (root != nullptr) {
        detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
        if (cJSON_IsString(detail) && detail->valuestring != nullptr) {
            SetLastError("HTTP %d: %s", response.http_status, detail->valuestring);
        } else if (cJSON_IsObject(detail)) {
            const cJSON* message =
                cJSON_GetObjectItemCaseSensitive(detail, "message");
            const cJSON* revision =
                cJSON_GetObjectItemCaseSensitive(detail, "current_revision");
            uint32_t current_revision = 0;
            if (JsonUnsigned(revision, &current_revision) &&
                current_revision <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                conflict_revision_ = static_cast<int>(current_revision);
            }
            if (cJSON_IsString(message) && message->valuestring != nullptr) {
                SetLastError("HTTP %d: %s", response.http_status, message->valuestring);
            }
        }
    }
    if (last_error_[0] == '\0') {
        const size_t printable = std::min(response.size, size_t{240});
        SetLastError("HTTP %d: %.*s", response.http_status,
                     static_cast<int>(printable),
                     response.data == nullptr
                         ? ""
                         : reinterpret_cast<const char*>(response.data));
    }
    cJSON_Delete(root);

    ESP_LOGE(kTag, "%s", last_error_);
    return response.http_status == 409 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t ServerClient::PostWav(
    const char* path,
    const uint8_t* wav,
    size_t wav_size,
    const char* extra_header_name,
    const char* extra_header_value,
    int timeout_ms,
    size_t max_response_size,
    ResponseBody* response)
{
    if (!Validate24kMonoPcm16Wav(wav, wav_size)) {
        SetLastError("audio must be a non-empty 24 kHz mono PCM16 WAV under 10 MB");
        return ESP_ERR_INVALID_ARG;
    }

    char prefix[256];
    const int prefix_size = std::snprintf(
        prefix, sizeof(prefix),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        kMultipartBoundary);
    char suffix[80];
    const int suffix_size = std::snprintf(
        suffix, sizeof(suffix), "\r\n--%s--\r\n", kMultipartBoundary);
    if (prefix_size <= 0 || static_cast<size_t>(prefix_size) >= sizeof(prefix) ||
        suffix_size <= 0 || static_cast<size_t>(suffix_size) >= sizeof(suffix)) {
        SetLastError("multipart body metadata is too large");
        return ESP_ERR_INVALID_SIZE;
    }

    char content_type[112];
    const int content_type_size = std::snprintf(
        content_type, sizeof(content_type),
        "multipart/form-data; boundary=%s", kMultipartBoundary);
    if (content_type_size <= 0 ||
        static_cast<size_t>(content_type_size) >= sizeof(content_type)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const BodyChunk chunks[] = {
        {reinterpret_cast<const uint8_t*>(prefix), static_cast<size_t>(prefix_size)},
        {wav, wav_size},
        {reinterpret_cast<const uint8_t*>(suffix), static_cast<size_t>(suffix_size)},
    };
    return PerformRequest(
        path, HTTP_METHOD_POST, content_type,
        max_response_size == kMaximumAudioResponseBytes
            ? "audio/wav"
            : "application/json",
        chunks, sizeof(chunks) / sizeof(chunks[0]),
        extra_header_name, extra_header_value,
        timeout_ms,
        max_response_size, response);
}

esp_err_t ServerClient::PostJson(
    const char* path, const char* json, ResponseBody* response)
{
    if (json == nullptr) {
        SetLastError("JSON body is missing");
        return ESP_ERR_INVALID_ARG;
    }
    const BodyChunk body = {
        reinterpret_cast<const uint8_t*>(json), std::strlen(json),
    };
    return PerformRequest(
        path, HTTP_METHOD_POST, "application/json", "application/json",
        &body, 1, nullptr, nullptr, kJsonTimeoutMs,
        kMaximumJsonResponseBytes, response);
}

esp_err_t ServerClient::VoiceChat(
    const uint8_t* wav,
    size_t wav_size,
    const char* session_id,
    AudioResponse* response)
{
    BeginRequest();
    if (response == nullptr) {
        SetLastError("voice chat response object is missing");
        return ESP_ERR_INVALID_ARG;
    }
    FreeAudioResponse(response);
    const char* effective_session =
        session_id == nullptr || session_id[0] == '\0'
            ? "esp32-default"
            : session_id;
    if (std::strlen(effective_session) > 128 || ContainsHeaderBreak(effective_session)) {
        SetLastError("chat session ID is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    ResponseBody body;
    const esp_err_t result = PostWav(
        "/voice-chat", wav, wav_size,
        "X-Session-ID", effective_session,
        kAudioTimeoutMs,
        kMaximumAudioResponseBytes, &body);
    response->http_status = last_http_status_;
    if (result == ESP_OK) {
        response->data = body.data;
        response->size = body.size;
        response->http_status = body.http_status;
    }
    return result;
}

esp_err_t ServerClient::Recognize(
    const uint8_t* wav,
    size_t wav_size,
    AsrResult* result,
    int timeout_ms)
{
    BeginRequest();
    if (result == nullptr || timeout_ms <= 0) {
        SetLastError("ASR result object or timeout is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    *result = {};

    ResponseBody body;
    esp_err_t error = PostWav(
        "/asr", wav, wav_size, nullptr, nullptr,
        timeout_ms,
        kMaximumJsonResponseBytes, &body);
    result->http_status = last_http_status_;
    if (error != ESP_OK) {
        return error;
    }

    cJSON* root = cJSON_ParseWithLength(
        reinterpret_cast<const char*>(body.data), body.size);
    const cJSON* text = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON* audio_seconds = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "audio_seconds");
    const cJSON* processing_seconds = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "processing_seconds");
    if (!cJSON_IsString(text) || text->valuestring == nullptr ||
        !CopyText(result->text, sizeof(result->text), text->valuestring) ||
        !cJSON_IsNumber(audio_seconds) || !cJSON_IsNumber(processing_seconds)) {
        SetLastError("ASR response JSON is invalid or text is too long");
        error = ESP_ERR_INVALID_RESPONSE;
    } else {
        result->audio_seconds = static_cast<float>(audio_seconds->valuedouble);
        result->processing_seconds = static_cast<float>(processing_seconds->valuedouble);
        result->http_status = body.http_status;
    }
    cJSON_Delete(root);
    heap_caps_free(body.data);
    return error;
}

esp_err_t ServerClient::ParseDictationState(
    const ResponseBody& response, DictationState* state)
{
    if (state == nullptr || response.data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = {};
    state->http_status = response.http_status;

    cJSON* root = cJSON_ParseWithLength(
        reinterpret_cast<const char*>(response.data), response.size);
    const cJSON* session_id = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON* mode = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON* status = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON* speech_text = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "speech_text");
    const cJSON* revision = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "revision");
    const cJSON* position = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "position");
    const cJSON* total_items = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "total_items");
    const cJSON* done = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "done");

    const bool valid =
        cJSON_IsString(session_id) && session_id->valuestring != nullptr &&
        CopyText(state->session_id, sizeof(state->session_id), session_id->valuestring) &&
        cJSON_IsString(mode) && mode->valuestring != nullptr &&
        CopyText(state->mode, sizeof(state->mode), mode->valuestring) &&
        cJSON_IsString(status) && status->valuestring != nullptr &&
        CopyText(state->status, sizeof(state->status), status->valuestring) &&
        cJSON_IsString(speech_text) && speech_text->valuestring != nullptr &&
        CopyTextTruncated(
            state->speech_text, sizeof(state->speech_text), speech_text->valuestring) &&
        JsonUnsigned(revision, &state->revision) &&
        JsonUnsigned(position, &state->position) &&
        JsonUnsigned(total_items, &state->total_items) &&
        cJSON_IsBool(done);
    if (valid) {
        state->done = cJSON_IsTrue(done);
    } else {
        SetLastError("dictation response JSON is invalid or a field is too long");
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ServerClient::CreateDictation(
    const DictationCreateRequest& request, DictationState* state)
{
    BeginRequest();
    if (state == nullptr || request.unit_id == nullptr || request.unit_id[0] == '\0' ||
        request.learner_id == nullptr || request.learner_id[0] == '\0' ||
        std::strlen(request.unit_id) > 160 || std::strlen(request.learner_id) > 128 ||
        !IsAllowedMode(request.mode) || request.count < 0 || request.count > 200 ||
        (request.random_seed != nullptr &&
         (request.random_seed[0] == '\0' || std::strlen(request.random_seed) > 128))) {
        SetLastError("dictation create arguments are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    *state = {};

    cJSON* root = cJSON_CreateObject();
    bool json_ok = root != nullptr &&
        cJSON_AddStringToObject(root, "learner_id", request.learner_id) != nullptr &&
        cJSON_AddStringToObject(root, "unit_id", request.unit_id) != nullptr &&
        cJSON_AddStringToObject(root, "mode", request.mode) != nullptr &&
        cJSON_AddBoolToObject(root, "shuffle", request.shuffle) != nullptr;
    if (json_ok && request.count > 0) {
        json_ok = cJSON_AddNumberToObject(root, "count", request.count) != nullptr;
    }
    if (json_ok && request.random_seed != nullptr) {
        json_ok = cJSON_AddStringToObject(
            root, "random_seed", request.random_seed) != nullptr;
    }
    char* json = json_ok ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (json == nullptr) {
        SetLastError("could not allocate dictation create JSON");
        return ESP_ERR_NO_MEM;
    }

    ResponseBody body;
    esp_err_t result = PostJson("/dictation/sessions", json, &body);
    cJSON_free(json);
    state->http_status = last_http_status_;
    if (result == ESP_OK) {
        result = ParseDictationState(body, state);
    }
    heap_caps_free(body.data);
    return result;
}

esp_err_t ServerClient::DictationAction(
    const char* session_id,
    const char* action,
    const char* request_id,
    uint32_t expected_revision,
    DictationState* state)
{
    BeginRequest();
    if (state == nullptr || !IsSafePathToken(session_id) ||
        !IsAllowedAction(action) || request_id == nullptr || request_id[0] == '\0' ||
        std::strlen(request_id) > 128) {
        SetLastError("dictation action arguments are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    *state = {};

    char path[224];
    const int path_size = std::snprintf(
        path, sizeof(path), "/dictation/sessions/%s/actions", session_id);
    if (path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(path)) {
        SetLastError("dictation action path is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON* root = cJSON_CreateObject();
    const bool json_ok = root != nullptr &&
        cJSON_AddStringToObject(root, "action", action) != nullptr &&
        cJSON_AddStringToObject(root, "request_id", request_id) != nullptr &&
        cJSON_AddNumberToObject(root, "expected_revision", expected_revision) != nullptr;
    char* json = json_ok ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (json == nullptr) {
        SetLastError("could not allocate dictation action JSON");
        return ESP_ERR_NO_MEM;
    }

    ResponseBody body;
    esp_err_t result = PostJson(path, json, &body);
    cJSON_free(json);
    state->http_status = last_http_status_;
    if (result == ESP_OK) {
        result = ParseDictationState(body, state);
    }
    heap_caps_free(body.data);
    return result;
}

esp_err_t ServerClient::GetDictationState(
    const char* session_id, DictationState* state)
{
    BeginRequest();
    if (state == nullptr || !IsSafePathToken(session_id)) {
        SetLastError("dictation state arguments are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    *state = {};

    char path[192];
    const int path_size = std::snprintf(
        path, sizeof(path), "/dictation/sessions/%s", session_id);
    if (path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(path)) {
        SetLastError("dictation state path is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    ResponseBody body;
    esp_err_t result = PerformRequest(
        path, HTTP_METHOD_GET, nullptr, "application/json",
        nullptr, 0, nullptr, nullptr, kJsonTimeoutMs,
        kMaximumJsonResponseBytes, &body);
    state->http_status = last_http_status_;
    if (result == ESP_OK) {
        result = ParseDictationState(body, state);
    }
    heap_caps_free(body.data);
    return result;
}

esp_err_t ServerClient::GetDictationPrompt(
    const char* session_id,
    uint32_t revision,
    AudioResponse* response)
{
    BeginRequest();
    if (response == nullptr || !IsSafePathToken(session_id)) {
        SetLastError("dictation prompt arguments are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    FreeAudioResponse(response);

    char path[240];
    const int path_size = std::snprintf(
        path, sizeof(path),
        "/dictation/sessions/%s/prompt.wav?revision=%lu",
        session_id, static_cast<unsigned long>(revision));
    if (path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(path)) {
        SetLastError("dictation prompt path is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    ResponseBody body;
    const esp_err_t result = PerformRequest(
        path, HTTP_METHOD_GET, nullptr, "audio/wav",
        nullptr, 0, nullptr, nullptr, kAudioTimeoutMs,
        kMaximumAudioResponseBytes, &body);
    response->http_status = last_http_status_;
    if (result == ESP_OK) {
        response->data = body.data;
        response->size = body.size;
        response->http_status = body.http_status;
    }
    return result;
}

}  // namespace companion
