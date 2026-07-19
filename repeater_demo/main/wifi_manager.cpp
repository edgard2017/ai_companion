#include "wifi_manager.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace wifi_manager {
namespace {

constexpr char kTag[] = "wifi_manager";
constexpr char kNvsNamespace[] = "repeater";
constexpr char kSetupSsid[] = "AI-Companion-Setup";
constexpr EventBits_t kConnected = BIT0;
constexpr TickType_t kConnectionTimeout = pdMS_TO_TICKS(20000);

struct DeviceConfig {
    char ssid[33] = {};
    char password[65] = {};
    char server_url[256] = {};
};

EventGroupHandle_t event_group;
bool reconnect_enabled = false;
char active_server_url[sizeof(DeviceConfig::server_url)] = CONFIG_REPEATER_SERVER_URL;

void WifiEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(event_group, kConnected);
        if (reconnect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(kTag, "Connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(event_group, kConnected);
    }
}

bool LoadConfig(DeviceConfig* config)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t ssid_size = sizeof(config->ssid);
    esp_err_t result = nvs_get_str(handle, "ssid", config->ssid, &ssid_size);
    if (result == ESP_OK) {
        size_t password_size = sizeof(config->password);
        if (nvs_get_str(handle, "password", config->password, &password_size) != ESP_OK) {
            config->password[0] = '\0';
        }
        size_t url_size = sizeof(config->server_url);
        if (nvs_get_str(handle, "server_url", config->server_url, &url_size) != ESP_OK) {
            std::snprintf(config->server_url, sizeof(config->server_url), "%s",
                          CONFIG_REPEATER_SERVER_URL);
        }
    }
    nvs_close(handle);
    return result == ESP_OK && config->ssid[0] != '\0';
}

esp_err_t SaveConfig(const DeviceConfig& config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    if ((result = nvs_set_str(handle, "ssid", config.ssid)) == ESP_OK &&
        (result = nvs_set_str(handle, "password", config.password)) == ESP_OK &&
        (result = nvs_set_str(handle, "server_url", config.server_url)) == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

bool ConnectToSavedNetwork(const DeviceConfig& config)
{
    wifi_config_t wifi_config = {};
    std::memcpy(wifi_config.sta.ssid, config.ssid,
                strnlen(config.ssid, sizeof(wifi_config.sta.ssid)));
    std::memcpy(wifi_config.sta.password, config.password,
                strnlen(config.password, sizeof(wifi_config.sta.password)));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    xEventGroupClearBits(event_group, kConnected);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    reconnect_enabled = true;
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(kTag, "Connecting to saved Wi-Fi '%s'", config.ssid);
    EventBits_t bits = xEventGroupWaitBits(
        event_group, kConnected, pdFALSE, pdTRUE, kConnectionTimeout);
    if ((bits & kConnected) != 0) {
        std::snprintf(active_server_url, sizeof(active_server_url), "%s",
                      config.server_url);
        return true;
    }

    ESP_LOGW(kTag, "Saved Wi-Fi was not reachable; starting setup mode");
    reconnect_enabled = false;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_stop());
    return false;
}

int HexValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

bool DecodeFormValue(
    const char* encoded, size_t encoded_size, char* output, size_t output_size)
{
    size_t written = 0;
    for (size_t index = 0; index < encoded_size; ++index) {
        char value = encoded[index];
        if (value == '+') {
            value = ' ';
        } else if (value == '%' && index + 2 < encoded_size) {
            int high = HexValue(encoded[index + 1]);
            int low = HexValue(encoded[index + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = static_cast<char>((high << 4) | low);
            index += 2;
        }
        if (written + 1 >= output_size) {
            return false;
        }
        output[written++] = value;
    }
    output[written] = '\0';
    return true;
}

bool GetFormValue(
    const char* body, const char* wanted_key, char* output, size_t output_size)
{
    const char* field = body;
    while (*field != '\0') {
        const char* field_end = std::strchr(field, '&');
        if (field_end == nullptr) {
            field_end = field + std::strlen(field);
        }
        const char* equals = static_cast<const char*>(
            std::memchr(field, '=', field_end - field));
        if (equals != nullptr &&
            static_cast<size_t>(equals - field) == std::strlen(wanted_key) &&
            std::memcmp(field, wanted_key, equals - field) == 0) {
            return DecodeFormValue(
                equals + 1, field_end - equals - 1, output, output_size);
        }
        field = *field_end == '\0' ? field_end : field_end + 1;
    }
    return false;
}

esp_err_t SetupPageHandler(httpd_req_t* request)
{
    static constexpr char page[] =
        "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AI Companion 配网</title><style>"
        "body{font-family:-apple-system,sans-serif;max-width:560px;margin:40px auto;padding:0 20px}"
        "label{display:block;margin-top:18px}input{box-sizing:border-box;width:100%;padding:12px;"
        "margin-top:6px;font-size:16px}button{width:100%;padding:13px;margin-top:24px;font-size:17px}"
        "small{color:#666}</style></head><body><h2>AI Companion 配网</h2>"
        "<p>填写家里的 Wi-Fi，一次保存，以后开机会自动连接。</p>"
        "<form method='post' action='/save'>"
        "<label>Wi-Fi 名称<input name='ssid' maxlength='32' required></label>"
        "<label>Wi-Fi 密码<input name='password' type='password' maxlength='64'></label>"
        "<label>Mac 语音服务地址<input name='server_url' maxlength='255' value='"
        CONFIG_REPEATER_SERVER_URL
        "' required></label><small>通常保持默认即可，Mac 换 IP 后仍能找到。</small>"
        "<button type='submit'>保存并连接</button></form></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

void RebootTask(void*)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

esp_err_t SavePageHandler(httpd_req_t* request)
{
    if (request->content_len <= 0 || request->content_len >= 2048) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form size");
    }
    char body[2048];
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        int result = httpd_req_recv(
            request, body + received, request->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }
    body[received] = '\0';

    DeviceConfig config;
    if (!GetFormValue(body, "ssid", config.ssid, sizeof(config.ssid)) ||
        config.ssid[0] == '\0' ||
        !GetFormValue(body, "password", config.password, sizeof(config.password)) ||
        !GetFormValue(body, "server_url", config.server_url, sizeof(config.server_url)) ||
        std::strncmp(config.server_url, "http://", 7) != 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi or URL");
    }

    esp_err_t result = SaveConfig(config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save setup: %s", esp_err_to_name(result));
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
    }

    static constexpr char saved[] =
        "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
        "<body style='font-family:-apple-system,sans-serif;padding:30px'>"
        "<h2>保存成功</h2><p>ESP 正在重启并连接家里 Wi-Fi，可以关闭这个页面了。</p>"
        "</body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send(request, saved, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(RebootTask, "setup_reboot", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

void StartSetupPortal()
{
    wifi_config_t access_point = {};
    std::snprintf(reinterpret_cast<char*>(access_point.ap.ssid),
                  sizeof(access_point.ap.ssid), "%s", kSetupSsid);
    access_point.ap.ssid_len = std::strlen(kSetupSsid);
    access_point.ap.channel = 1;
    access_point.ap.max_connection = 4;
    access_point.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access_point));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = SetupPageHandler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = SavePageHandler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));

    ESP_LOGW(kTag, "Setup mode: connect phone to '%s' (no password)", kSetupSsid);
    ESP_LOGW(kTag, "Then open http://192.168.4.1 in a browser");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

esp_err_t Initialize()
{
    event_group = xEventGroupCreate();
    if (event_group == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, WifiEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventHandler, nullptr));

    DeviceConfig config;
    if (LoadConfig(&config) && ConnectToSavedNetwork(config)) {
        ESP_LOGI(kTag, "Mac endpoint: %s", active_server_url);
        return ESP_OK;
    }

    StartSetupPortal();
    return ESP_FAIL;
}

void WaitUntilConnected()
{
    xEventGroupWaitBits(event_group, kConnected, pdFALSE, pdTRUE, portMAX_DELAY);
}

const char* ServerUrl()
{
    return active_server_url;
}

}  // namespace wifi_manager
