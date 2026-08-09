#include "protocol.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "Protocol";
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = std::move(callback);
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = std::move(callback);
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = std::move(callback);
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = std::move(callback);
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = std::move(callback);
}

void Protocol::OnConnected(std::function<void()> callback) {
    on_connected_ = std::move(callback);
}

void Protocol::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = std::move(callback);
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    SendText("{\"session_id\":\"" + session_id_ +
             "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}");
}

void Protocol::SendStartListening(ListeningMode mode) {
    const char* mode_name = mode == kListeningModeRealtime ? "realtime"
                            : mode == kListeningModeAutoStop ? "auto"
                                                             : "manual";
    SendText("{\"session_id\":\"" + session_id_ +
             "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"" + mode_name + "\"}");
}

void Protocol::SendStopListening() {
    SendText("{\"session_id\":\"" + session_id_ +
             "\",\"type\":\"listen\",\"state\":\"stop\"}");
}

void Protocol::SendMcpMessage(const std::string& payload) {
    SendText("{\"session_id\":\"" + session_id_ +
             "\",\"type\":\"mcp\",\"payload\":" + payload + "}");
}

bool Protocol::IsTimeout() const {
    constexpr int kTimeoutSeconds = 120;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_incoming_time_);
    if (elapsed.count() > kTimeoutSeconds) {
        ESP_LOGE(kTag, "Channel timeout %ld seconds", static_cast<long>(elapsed.count()));
        return true;
    }
    return false;
}
