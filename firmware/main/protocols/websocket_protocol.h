#ifndef WEBSOCKET_PROTOCOL_H
#define WEBSOCKET_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol() override;

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    static constexpr EventBits_t kServerHelloEvent = BIT0;

    EventGroupHandle_t event_group_ = nullptr;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    bool configuration_discovered_ = false;

    bool SendText(const std::string& text) override;
    bool DiscoverConfiguration();
    std::string GetHelloMessage() const;
    bool ParseServerHello(const cJSON* root);
    void HandleBinaryMessage(const char* data, size_t len);
};

#endif  // WEBSOCKET_PROTOCOL_H
