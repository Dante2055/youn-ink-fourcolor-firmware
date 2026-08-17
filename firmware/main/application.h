#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "audio_service.h"
#include "device_state.h"

namespace ui {
class RawDrawUiManager;
}

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_.load(std::memory_order_acquire); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void PlaySound(const std::string_view& sound);
    void PlaySound(const std::string_view& sound, int duration_ms);
    void MuteSound();
    void StopSound();
    bool CanEnterSleepMode() const;

    AudioService& GetAudioService() { return audio_service_; }
    ui::RawDrawUiManager* GetRawDrawUiManager() { return rawdraw_ui_manager_.get(); }
    void UpdateStatusBarForUi();
    void OnUpClick();
    void OnDownClick();
    void OnUpLongPress();
    void OnDownLongPress();
    void OnWifiConfigComboLongPress();
    void OnBootClick();
    void OnBootDoubleClick();
    void OnBootLongPress();
    void OnBootRelease();
    void OnClockSynchronized();

private:
    Application();
    ~Application();

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<bool> wifi_connected_{false};
    std::atomic<bool> protocol_connected_{false};
    std::atomic<bool> protocol_connecting_{false};
    std::atomic<bool> conversation_active_{false};
    std::atomic<bool> resume_listening_{false};
    std::atomic<bool> silent_boot_refresh_{false};
    std::atomic<bool> lan_user_disabled_{false};  ///< User manually closed LAN; don't auto-restart on WiFi reconnect
    AudioService audio_service_;
    std::unique_ptr<ui::RawDrawUiManager> rawdraw_ui_manager_;
    std::unique_ptr<Protocol> protocol_;
    esp_timer_handle_t sleep_timer_ = nullptr;
    esp_timer_handle_t silent_boot_fallback_timer_ = nullptr;
    std::mutex scheduled_mutex_;
    std::deque<std::function<void()>> scheduled_callbacks_;

    void InitializeDialogueProtocol(AudioCodec* codec);
    void StartListening();
    void StopListening();
    void BeginListening();
    void PrepareDialogueConnection();
    void ConnectAndStartListening();
    void HandleProtocolJson(const cJSON* root);
    void ArmSyncSleepTimer();
    void EnterScheduledSleep();
    void EnterManualSleep();
    void NoteButtonActivity();
    void EnterWifiConfigMode();
};

#endif  // _APPLICATION_H_
