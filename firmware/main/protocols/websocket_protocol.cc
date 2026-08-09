#include "websocket_protocol.h"

#include "audio_service.h"
#include "board.h"
#include "network_interface.h"
#include "settings.h"
#include "system_info.h"

#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "WS";

bool IsWebsocketUrl(const char* url) {
    return url != nullptr &&
           (std::strncmp(url, "ws://", 5) == 0 || std::strncmp(url, "wss://", 6) == 0);
}
}

WebsocketProtocol::WebsocketProtocol() : event_group_(xEventGroupCreate()) {}

WebsocketProtocol::~WebsocketProtocol() {
    websocket_.reset();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool WebsocketProtocol::Start() {
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet || !websocket_ || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string frame(sizeof(BinaryProtocol2) + packet->payload.size(), '\0');
        auto* header = reinterpret_cast<BinaryProtocol2*>(frame.data());
        header->version = htons(version_);
        header->type = 0;
        header->reserved = 0;
        header->timestamp = htonl(packet->timestamp);
        header->payload_size = htonl(packet->payload.size());
        std::memcpy(header->payload, packet->payload.data(), packet->payload.size());
        return websocket_->Send(frame.data(), frame.size(), true);
    }

    if (version_ == 3) {
        std::string frame(sizeof(BinaryProtocol3) + packet->payload.size(), '\0');
        auto* header = reinterpret_cast<BinaryProtocol3*>(frame.data());
        header->type = 0;
        header->reserved = 0;
        header->payload_size = htons(packet->payload.size());
        std::memcpy(header->payload, packet->payload.data(), packet->payload.size());
        return websocket_->Send(frame.data(), frame.size(), true);
    }

    return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (!websocket_ || !websocket_->IsConnected()) {
        return false;
    }
    if (!websocket_->Send(text)) {
        ESP_LOGE(kTag, "Failed to send websocket text frame");
        SetError("服务器发送失败");
        return false;
    }
    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    const bool was_open = websocket_ && websocket_->IsConnected();
    websocket_.reset();
    session_id_.clear();
    if (was_open && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    std::string url;
    std::string token;
    auto load_configuration = [this, &url, &token]() {
        Settings settings("websocket", false);
        url = settings.GetString("url");
        token = settings.GetString("token");
        const int configured_version = settings.GetInt("version", 1);
        version_ = configured_version >= 1 && configured_version <= 3 ? configured_version : 1;
    };

    load_configuration();
    if (!configuration_discovered_) {
        configuration_discovered_ = DiscoverConfiguration();
        if (configuration_discovered_) {
            load_configuration();
        } else if (!url.empty()) {
            ESP_LOGW(kTag, "OTA configuration refresh failed; using cached websocket settings");
        }
    }

    if (url.empty()) {
        ESP_LOGE(kTag, "Websocket configuration is unavailable");
        SetError("无法获取 WebSocket 配置");
        return false;
    }

    error_occurred_ = false;
    session_id_.clear();
    xEventGroupClearBits(event_group_, kServerHelloEvent);

    auto* network = Board::GetInstance().GetNetwork();
    websocket_ = network ? network->CreateWebSocket(1) : nullptr;
    if (!websocket_) {
        SetError("无法创建 WebSocket");
        return false;
    }

    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (binary) {
            HandleBinaryMessage(data, len);
        } else {
            cJSON* root = cJSON_ParseWithLength(data, len);
            if (!root) {
                ESP_LOGE(kTag, "Invalid websocket JSON (%u bytes)", static_cast<unsigned>(len));
                return;
            }
            const cJSON* type = cJSON_GetObjectItem(root, "type");
            if (!cJSON_IsString(type)) {
                ESP_LOGE(kTag, "Websocket JSON is missing type");
            } else if (std::strcmp(type->valuestring, "hello") == 0) {
                ParseServerHello(root);
            } else if (on_incoming_json_) {
                on_incoming_json_(root);
            }
            cJSON_Delete(root);
        }
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(kTag, "Websocket disconnected");
        if (on_disconnected_) {
            on_disconnected_();
        }
        if (on_audio_channel_closed_) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(kTag, "Connecting to websocket server: %s (protocol %d)", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(kTag, "Websocket connect failed, code=%d", websocket_->GetLastError());
        SetError("服务器连接失败");
        websocket_.reset();
        return false;
    }

    if (!SendText(GetHelloMessage())) {
        websocket_.reset();
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        event_group_, kServerHelloEvent, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & kServerHelloEvent)) {
        ESP_LOGE(kTag, "Timed out waiting for server hello");
        SetError("服务器握手超时");
        websocket_.reset();
        return false;
    }

    if (on_connected_) {
        on_connected_();
    }
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

bool WebsocketProtocol::DiscoverConfiguration() {
    Settings wifi_settings("wifi", false);
    const std::string ota_url = wifi_settings.GetString("ota_url", CONFIG_OTA_URL);
    if (ota_url.empty()) {
        ESP_LOGE(kTag, "OTA configuration URL is empty");
        return false;
    }

    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        ESP_LOGE(kTag, "Failed to create OTA configuration HTTP client");
        return false;
    }

    http->SetTimeout(15000);
    http->SetHeader("Activation-Version", "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", board.GetUuid());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Accept-Language", "zh-CN");
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(board.GetSystemInfoJson());

    ESP_LOGI(kTag, "Discovering websocket configuration from %s", ota_url.c_str());
    if (!http->Open("POST", ota_url)) {
        ESP_LOGE(kTag, "OTA configuration request failed, code=%d", http->GetLastError());
        return false;
    }
    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(kTag, "OTA configuration returned HTTP %d", status_code);
        http->Close();
        return false;
    }

    const std::string response = http->ReadAll();
    http->Close();
    cJSON* root = cJSON_ParseWithLength(response.data(), response.size());
    if (!root) {
        ESP_LOGE(kTag, "OTA configuration returned invalid JSON (%u bytes)",
                 static_cast<unsigned>(response.size()));
        return false;
    }

    const cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    const cJSON* url = cJSON_IsObject(websocket) ? cJSON_GetObjectItem(websocket, "url") : nullptr;
    const cJSON* token = cJSON_IsObject(websocket) ? cJSON_GetObjectItem(websocket, "token") : nullptr;
    const cJSON* version = cJSON_IsObject(websocket) ? cJSON_GetObjectItem(websocket, "version") : nullptr;
    if (!cJSON_IsString(url) || !IsWebsocketUrl(url->valuestring)) {
        ESP_LOGE(kTag, "OTA configuration is missing a valid websocket.url");
        cJSON_Delete(root);
        return false;
    }

    int protocol_version = cJSON_IsNumber(version) ? version->valueint : 1;
    if (protocol_version < 1 || protocol_version > 3) {
        protocol_version = 1;
    }
    {
        Settings settings("websocket", true);
        settings.SetString("url", url->valuestring);
        settings.SetString("token", cJSON_IsString(token) ? token->valuestring : "");
        settings.SetInt("version", protocol_version);
    }
    ESP_LOGI(kTag, "Websocket configuration saved: url=%s version=%d",
             url->valuestring, protocol_version);
    cJSON_Delete(root);
    return true;
}

std::string WebsocketProtocol::GetHelloMessage() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON_AddStringToObject(root, "transport", "websocket");

    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", false);
    cJSON_AddItemToObject(root, "features", features);

    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio);

    char* json = cJSON_PrintUnformatted(root);
    std::string message = json ? json : "";
    cJSON_free(json);
    cJSON_Delete(root);
    return message;
}

bool WebsocketProtocol::ParseServerHello(const cJSON* root) {
    const cJSON* transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || std::strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(kTag, "Unsupported or missing server transport");
        return false;
    }

    const cJSON* session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
    }

    const cJSON* audio = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio)) {
        const cJSON* sample_rate = cJSON_GetObjectItem(audio, "sample_rate");
        const cJSON* frame_duration = cJSON_GetObjectItem(audio, "frame_duration");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    ESP_LOGI(kTag, "Server hello: session=%s audio=%dHz/%dms",
             session_id_.c_str(), server_sample_rate_, server_frame_duration_);
    xEventGroupSetBits(event_group_, kServerHelloEvent);
    return true;
}

void WebsocketProtocol::HandleBinaryMessage(const char* data, size_t len) {
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(data);
    size_t payload_size = len;
    uint32_t timestamp = 0;

    if (version_ == 2) {
        if (len < sizeof(BinaryProtocol2)) {
            ESP_LOGE(kTag, "Short protocol-v2 audio frame: %u", static_cast<unsigned>(len));
            return;
        }
        const auto* header = reinterpret_cast<const BinaryProtocol2*>(data);
        payload_size = ntohl(header->payload_size);
        if (payload_size > len - sizeof(BinaryProtocol2)) {
            ESP_LOGE(kTag, "Invalid protocol-v2 audio payload: %u", static_cast<unsigned>(payload_size));
            return;
        }
        timestamp = ntohl(header->timestamp);
        payload = header->payload;
    } else if (version_ == 3) {
        if (len < sizeof(BinaryProtocol3)) {
            ESP_LOGE(kTag, "Short protocol-v3 audio frame: %u", static_cast<unsigned>(len));
            return;
        }
        const auto* header = reinterpret_cast<const BinaryProtocol3*>(data);
        payload_size = ntohs(header->payload_size);
        if (payload_size > len - sizeof(BinaryProtocol3)) {
            ESP_LOGE(kTag, "Invalid protocol-v3 audio payload: %u", static_cast<unsigned>(payload_size));
            return;
        }
        payload = header->payload;
    }

    if (on_incoming_audio_) {
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = server_sample_rate_,
            .frame_duration = server_frame_duration_,
            .timestamp = timestamp,
            .payload = std::vector<uint8_t>(payload, payload + payload_size),
        }));
    }
}
