#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/photo_storage.h"
#include "common/storage_manager.h"
#include "common/weather_api.h"
#include "display.h"
#include "protocols/websocket_protocol.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"

#include <esp_mac.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/gpio.h>

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <utility>

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
// Indices into the settings item list built in Initialize(); keep in sync
// with the push_back order there.
constexpr int kSettingsSlideshowIndex = 4;
constexpr int kSettingsWifiIndex = 6;
constexpr int kSettingsHttpServerIndex = 7;
constexpr int kSettingsLanIpIndex = 8;

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return "关闭";
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? "关闭" : "开启";
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

void UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return;
    renderer->UpdateChecked(kSettingsWifiIndex, connected);
    renderer->UpdateItem(kSettingsWifiIndex, value ? value : (connected ? "已连接" : "未连接"));
}

void UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? "已开启" : "已关闭";
    }
    renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    renderer->UpdateItem(kSettingsHttpServerIndex, value);
}

void UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return;
    renderer->UpdateItem(kSettingsLanIpIndex, ip_address.empty() ? "未获取" : ip_address);
}

void StartSntpClockSyncOnce() {
    static bool s_started = false;
    if (s_started) return;

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().OnClockSynchronized();
    });
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=Asia/Shanghai servers=ntp.aliyun.com,cn.pool.ntp.org,pool.ntp.org");
}

bool IsLocalHttpServiceRunning(const ui::RawDrawUiManager* manager) {
    return manager != nullptr && manager->IsHttpServerRunning();
}

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItem(object, key);
    if (cJSON_IsString(value)) return value->valuestring;
    if (!cJSON_IsNumber(value)) return "";
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%g", value->valuedouble);
    return buffer;
}

int32_t JsonInt(const cJSON* object, const char* key, int32_t fallback = 0) {
    const cJSON* value = cJSON_GetObjectItem(object, key);
    if (cJSON_IsNumber(value)) return static_cast<int32_t>(value->valuedouble);
    if (cJSON_IsString(value)) {
        char* end = nullptr;
        const long parsed = strtol(value->valuestring, &end, 10);
        if (end != value->valuestring && *end == '\0') return static_cast<int32_t>(parsed);
    }
    return fallback;
}

std::string JsonSignature(const cJSON* object) {
    char* json = cJSON_PrintUnformatted(object);
    if (!json) return "";
    std::string signature(json);
    cJSON_free(json);
    return signature;
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
        esp_timer_delete(sleep_timer_);
        sleep_timer_ = nullptr;
    }
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    // BOOT-key wake from deep sleep starts a service-off session (quick
    // interactive use). A real restart (settings "重启") has no deep-sleep
    // wakeup cause, so the LAN service defaults back to on.
    const bool woke_by_boot_button =
        esp_reset_reason() == ESP_RST_DEEPSLEEP &&
        (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0)) != 0;
    if (woke_by_boot_button) {
        lan_user_disabled_.store(true, std::memory_order_release);
        ESP_LOGI(kTag, "BOOT wake: LAN service stays off until restart or manual enable");
    }
    SetDeviceState(kDeviceStateStarting);

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, bool urgent) {
        if (urgent) {
            lcd->RequestUrgentFullRefresh();
        } else {
            lcd->RequestUrgentRefresh();
        }
    });
    rawdraw_ui_manager_->SetLanServiceClosedCallback([this]() {
        lan_user_disabled_.store(true, std::memory_order_release);
        ESP_LOGI(kTag, "LAN service closed via web control; auto-start disabled until reboot");
        Schedule([this]() {
            if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                UpdateHttpServerSettingsItem(sr, false);
            }
            UpdateStatusBarForUi();
        });
    });
    rawdraw_ui_manager_->SetPageSwitchCallback([this](ui::RawDrawPageId page) {
        if (page != ui::RawDrawPageId::Chat) {
            if (conversation_active_.exchange(false, std::memory_order_acq_rel)) {
                resume_listening_.store(false, std::memory_order_release);
                Schedule([this]() { StopListening(); });
            }
            ArmSyncSleepTimer();
            return;
        }
        if (page == ui::RawDrawPageId::Chat) {
            if (conversation_active_.exchange(true, std::memory_order_acq_rel)) return;
            resume_listening_.store(false, std::memory_order_release);
            Schedule([this]() { StartListening(); });
        }
    });

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        Settings gallery_nvs(kGalleryNamespace, false);
        int slideshow_interval = gallery_nvs.GetInt(kSlideshowIntervalKey, 0);
        if (slideshow_interval != 0 && slideshow_interval != 5 &&
            slideshow_interval != 10 && slideshow_interval != 30) {
            slideshow_interval = 0;
        }
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());
        rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval);

        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({"应用", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"待办事项", "打开", nullptr, rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetCurrentPageWithoutRender(ui::RawDrawPageId::Todo);
                             }
                         }});
        items.push_back({"日历", "打开", nullptr, rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetCurrentPageWithoutRender(ui::RawDrawPageId::Calendar);
                             }
                         }});
        items.push_back({"相册", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"轮播间隔", FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kGalleryNamespace, true);
                             const int current = nvs.GetInt(kSlideshowIntervalKey, 0);
                             const int next = NextSlideshowInterval(current);
                             nvs.SetInt(kSlideshowIntervalKey, next);
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(next);
                             }
                             if (next > 0 && sleep_timer_ != nullptr) {
                                 esp_timer_stop(sleep_timer_);
                                 ESP_LOGI(kTag, "Sync sleep timer paused while gallery slideshow is enabled");
                             } else if (next <= 0) {
                                 ArmSyncSleepTimer();
                             }
                             sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(next));
                         }});
        items.push_back({"网络", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Wi-Fi", "未连接", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             auto& wifi = WifiManager::GetInstance();
                            if (wifi_connected_.load(std::memory_order_acquire) || wifi.IsConnected()) {
                                ESP_LOGI(kTag, "Wi-Fi setting toggled OFF");
                                lan_user_disabled_.store(true, std::memory_order_release);
                                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                    rawdraw_ui_manager_->StopLanHttpServer();
                                    UpdateHttpServerSettingsItem(sr, false);
                                }
                                 wifi.StopStation();
                                 wifi_connected_.store(false, std::memory_order_release);
                                 UpdateWifiSettingsItem(sr, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 UpdateWifiSettingsItem(sr, false, "连接中");
                                 wifi.StartStation();
                             }
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网服务", "已关闭", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                 ESP_LOGI(kTag, "LAN HTTP server toggled OFF");
                                 lan_user_disabled_.store(true, std::memory_order_release);
                                 rawdraw_ui_manager_->StopLanHttpServer();
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateStatusBarForUi();
                                 ArmSyncSleepTimer();
                                 return;
                             }

                             auto& wifi = WifiManager::GetInstance();
                             if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
                                 UpdateHttpServerSettingsItem(sr, false, "需先连接WiFi");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const std::string ip = wifi.GetIpAddress();
                             if (ip.empty()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires station IP");
                                 UpdateHttpServerSettingsItem(sr, false, "等待IP");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             lan_user_disabled_.store(false, std::memory_order_release);
                             const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                             ESP_LOGI(kTag, "LAN HTTP server toggled ON: started=%d url=http://%s/",
                                      started ? 1 : 0, ip.c_str());
                             if (started && sleep_timer_ != nullptr) {
                                 esp_timer_stop(sleep_timer_);
                                 ESP_LOGI(kTag, "Sync sleep timer paused while LAN HTTP server is running");
                             }
                             UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                             UpdateLanIpSettingsItem(sr, started ? ip : WifiManager::GetInstance().GetIpAddress());
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网IP", "未获取", nullptr, rawdraw::SettingsItemType::Normal, false});
        items.push_back({"省电模式", "手动进入", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Manual sleep requested from settings");
                             EnterManualSleep();
                         }});
        items.push_back({"系统", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"重启", "执行", nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({"关于", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"固件", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    InitializeDialogueProtocol(codec);

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    silent_boot_refresh_.store(true, std::memory_order_release);
    if (silent_boot_fallback_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            if (app->silent_boot_refresh_.exchange(false, std::memory_order_acq_rel)) {
                ESP_LOGI("Application", "Silent boot timeout fallback: performing single final refresh");
                app->UpdateStatusBarForUi();
            }
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "boot_fallback";
        esp_timer_create(&args, &silent_boot_fallback_timer_);
    }
    if (silent_boot_fallback_timer_ != nullptr) {
        esp_timer_stop(silent_boot_fallback_timer_);
        esp_timer_start_once(silent_boot_fallback_timer_, 6000000);  // 6s fallback
    }

    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(kTag, "Wake from deep sleep: flash activity LED (silent network sync active)");
        board.FlashActivityLed();
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                wifi_connected_.store(true, std::memory_order_release);
                StartSntpClockSyncOnce();
                if (conversation_active_.load(std::memory_order_acquire)) {
                    Schedule([this]() { StartListening(); });
                }
                if (lan_user_disabled_.load(std::memory_order_acquire)) {
                    ESP_LOGI(kTag, "LAN HTTP server auto-start skipped: manually disabled by user");
                } else if (rawdraw_ui_manager_ && !rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    const std::string ip = data.empty() ? WifiManager::GetInstance().GetIpAddress() : data;
                    if (!ip.empty()) {
                        const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                        ESP_LOGI(kTag, "LAN HTTP server auto-start after WiFi: started=%d url=http://%s/",
                                 started ? 1 : 0, ip.c_str());
                        if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                            UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                            UpdateLanIpSettingsItem(sr, ip);
                        }
                    }
                }
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                UpdateStatusBarForUi();
                ArmSyncSleepTimer();
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    rawdraw_ui_manager_->StopLanHttpServer();
                }
                UpdateStatusBarForUi();
                ArmSyncSleepTimer();
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi config AP exited, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                      std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
    });

    // Start network (non-blocking, WiFi connects asynchronously)
    board.RequestNetwork();

    SetDeviceState(kDeviceStateIdle);

    // ---- Light sleep configuration ----
    // Enable automatic light sleep: when all FreeRTOS tasks are blocked,
    // the CPU enters light sleep and wakes on GPIO interrupt or esp_timer.
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;  // 240 MHz when active
    pm_config.min_freq_mhz = CONFIG_XTAL_FREQ;                 // XTAL (40 MHz) when idle
    pm_config.light_sleep_enable = true;
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if (pm_err == ESP_OK) {
        ESP_LOGI(kTag, "Auto light sleep enabled (max=%dMHz, min=%dMHz)",
                 pm_config.max_freq_mhz, pm_config.min_freq_mhz);
    } else {
        ESP_LOGW(kTag, "Failed to configure PM: %s", esp_err_to_name(pm_err));
    }

    // Configure button GPIOs as light sleep wakeup sources
    const gpio_num_t wake_gpios[] = {
        static_cast<gpio_num_t>(BOOT_BUTTON_GPIO),       // GPIO 0  - BOOT/confirm
        static_cast<gpio_num_t>(TODO_UP_BUTTON_GPIO),    // GPIO 39 - UP
        static_cast<gpio_num_t>(TODO_DOWN_BUTTON_GPIO),  // GPIO 18 - DOWN
    };
    for (auto pin : wake_gpios) {
        gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
    ESP_LOGI(kTag, "GPIO wakeup enabled for buttons: %d, %d, %d",
             BOOT_BUTTON_GPIO, TODO_UP_BUTTON_GPIO, TODO_DOWN_BUTTON_GPIO);
#endif  // CONFIG_PM_ENABLE
}

void Application::OnUpClick() {
    ESP_LOGI(kTag, "UP click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnDownClick() {
    ESP_LOGI(kTag, "DOWN click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

void Application::OnUpLongPress() {
    ESP_LOGI(kTag, "UP long press");
    NoteButtonActivity();
    if (!rawdraw_ui_manager_) return;

    const auto current_page = rawdraw_ui_manager_->GetCurrentPage();
    const auto target_page =
        (current_page == ui::RawDrawPageId::Chat || current_page == ui::RawDrawPageId::Settings)
            ? ui::RawDrawPageId::Gallery
            : ui::RawDrawPageId::Chat;
    rawdraw_ui_manager_->SwitchPage(target_page);
}

void Application::OnDownLongPress() {
    ESP_LOGI(kTag, "DOWN long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    ESP_LOGI(kTag, "BOOT click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Chat &&
        GetDeviceState() == kDeviceStateIdle) {
        resume_listening_.store(false, std::memory_order_release);
        Schedule([this]() { StartListening(); });
        return;
    }
    if (rawdraw_ui_manager_) {
        const auto page = rawdraw_ui_manager_->GetCurrentPage();
        if (page == ui::RawDrawPageId::Ebook) {
            auto* ebook = rawdraw_ui_manager_->GetEbookRenderer();
            if (ebook && !ebook->IsReaderMode()) {
                const std::string filename = ebook->GetSelectedFile();
                if (filename.empty()) return;
                const std::string content = storage_manager::ReadTxtFile(filename);
                if (!content.empty()) {
                    ebook->OpenFile(filename, content);
                    rawdraw_ui_manager_->RequestActivePageRefresh();
                }
                return;
            }
        }
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootDoubleClick() {
    ESP_LOGI(kTag, "BOOT double click");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        const auto current_page = rawdraw_ui_manager_->GetCurrentPage();
        if (current_page == ui::RawDrawPageId::Chat) {
            ESP_LOGI(kTag, "BOOT double click on Chat - triggering voice listening");
            resume_listening_.store(false, std::memory_order_release);
            Schedule([this]() { StartListening(); });
            return;
        }
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootDoubleClick});
    }
}

void Application::OnBootLongPress() {
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
        }
        WifiManager::GetInstance().StartStation();
        return;
    }

    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::OnBootRelease() {
}

void Application::OnClockSynchronized() {
    if (silent_boot_fallback_timer_ != nullptr) {
        esp_timer_stop(silent_boot_fallback_timer_);
    }
    const bool was_silent = silent_boot_refresh_.exchange(false, std::memory_order_acq_rel);
    if (was_silent) {
        ESP_LOGI(kTag, "Silent boot WiFi & SNTP clock sync complete: performing single final screen refresh");
    }
    UpdateStatusBarForUi();
    if (conversation_active_.load(std::memory_order_acquire)) {
        Schedule([this]() { StartListening(); });
    }
}

void Application::InitializeDialogueProtocol(AudioCodec* codec) {
    protocol_ = std::make_unique<WebsocketProtocol>();

    AudioServiceCallbacks audio_callbacks;
    audio_callbacks.on_send_queue_available = [this]() {
        if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
            while (audio_service_.PopPacketFromSendQueue()) {
            }
            return;
        }
        while (auto packet = audio_service_.PopPacketFromSendQueue()) {
            if (!protocol_->SendAudio(std::move(packet))) {
                break;
            }
        }
    };
    audio_service_.SetCallbacks(audio_callbacks);

    protocol_->OnNetworkError([this](const std::string& message) {
        Schedule([this, message]() {
            protocol_connected_.store(false, std::memory_order_release);
            SetDeviceState(kDeviceStateIdle);
            ESP_LOGW(kTag, "Dialogue network error: %s", message.c_str());
            UpdateStatusBarForUi();
        });
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    });
    protocol_->OnIncomingJson([this](const cJSON* root) {
        HandleProtocolJson(root);
    });
    protocol_->OnAudioChannelOpened([this, codec]() {
        Schedule([this, codec]() {
            protocol_connected_.store(true, std::memory_order_release);
            if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
                ESP_LOGW(kTag, "Server audio is %d Hz; codec output is %d Hz",
                         protocol_->server_sample_rate(), codec->output_sample_rate());
            }
            UpdateStatusBarForUi();
        });
    });
    protocol_->OnAudioChannelClosed([this]() {
        Schedule([this]() {
            protocol_connected_.store(false, std::memory_order_release);
            protocol_connecting_.store(false, std::memory_order_release);
            resume_listening_.store(false, std::memory_order_release);
            audio_service_.EnableVoiceProcessing(false);
            SetDeviceState(kDeviceStateIdle);
            UpdateStatusBarForUi();
        });
    });

    protocol_->Start();
}

void Application::StartListening() {
    if (!protocol_ || !conversation_active_.load(std::memory_order_acquire)) {
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        BeginListening();
        return;
    }
    PrepareDialogueConnection();
}

void Application::PrepareDialogueConnection() {
    time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    if (!protocol_) {
        ESP_LOGD(kTag, "PrepareDialogue: no protocol");
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        ESP_LOGD(kTag, "PrepareDialogue: audio channel already open");
        return;
    }
    if (local_tm.tm_year + 1900 < 2020) {
        ESP_LOGD(kTag, "PrepareDialogue: system time not synced (year=%d)", local_tm.tm_year + 1900);
        return;
    }
    if (!conversation_active_.load(std::memory_order_acquire)) {
        ESP_LOGD(kTag, "PrepareDialogue: conversation not active");
        return;
    }
    if (!wifi_connected_.load(std::memory_order_acquire) &&
        !WifiManager::GetInstance().IsConnected()) {
        ESP_LOGD(kTag, "PrepareDialogue: WiFi not connected");
        return;
    }
    if (protocol_connecting_.exchange(true, std::memory_order_acq_rel)) {
        ESP_LOGD(kTag, "PrepareDialogue: already connecting");
        return;
    }

    SetDeviceState(kDeviceStateConnecting);
    const BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            app->ConnectAndStartListening();
            vTaskDelete(nullptr);
        },
        "chat_connect", 6144, this, 3, nullptr);
    if (created != pdPASS) {
        protocol_connecting_.store(false, std::memory_order_release);
        SetDeviceState(kDeviceStateIdle);
        ESP_LOGE(kTag, "Unable to start dialogue connection task");
    }
}

void Application::ConnectAndStartListening() {
    const bool opened = protocol_ && protocol_->OpenAudioChannel();
    Schedule([this, opened]() {
        protocol_connecting_.store(false, std::memory_order_release);
        if (!opened) {
            SetDeviceState(kDeviceStateIdle);
            return;
        }
        if (!conversation_active_.load(std::memory_order_acquire)) {
            SetDeviceState(kDeviceStateIdle);
            return;
        }
        BeginListening();
    });
}

void Application::BeginListening() {
    if (!protocol_ || !protocol_->IsAudioChannelOpened() ||
        !conversation_active_.load(std::memory_order_acquire)) {
        return;
    }
    if (GetDeviceState() == kDeviceStateSpeaking) {
        protocol_->SendAbortSpeaking(kAbortReasonNone);
        audio_service_.ResetDecoder();
    }

    audio_service_.MarkPttStart(esp_timer_get_time() / 1000);
    audio_service_.EnableVoiceProcessing(true);
    protocol_->SendStartListening(kListeningModeAutoStop);
    SetDeviceState(kDeviceStateListening);
}

void Application::StopListening() {
    resume_listening_.store(false, std::memory_order_release);
    audio_service_.EnableVoiceProcessing(false);
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            protocol_->SendAbortSpeaking(kAbortReasonNone);
            audio_service_.ResetDecoder();
        } else if (GetDeviceState() == kDeviceStateListening) {
            protocol_->SendStopListening();
        }
    }
    SetDeviceState(kDeviceStateIdle);
}

void Application::HandleProtocolJson(const cJSON* root) {
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }

    const cJSON* payload = root;
    const char* message_type = type->valuestring;
    if (strcmp(message_type, "custom") == 0) {
        payload = cJSON_GetObjectItem(root, "payload");
        if (!cJSON_IsObject(payload)) return;
        const cJSON* payload_type = cJSON_GetObjectItem(payload, "type");
        if (!cJSON_IsString(payload_type)) return;
        message_type = payload_type->valuestring;
    }
    const cJSON* data_object = cJSON_GetObjectItem(payload, "data");
    if (cJSON_IsObject(data_object)) payload = data_object;

    if (strcmp(message_type, "weather") == 0) {
        const std::string signature = JsonSignature(payload);
        WeatherData data;
        data.city = JsonString(payload, "city");
        data.temp = JsonString(payload, "temp");
        data.feels_like = JsonString(payload, "feels_like");
        data.weather_icon = JsonString(payload, "weather_icon");
        data.weather_text = JsonString(payload, "weather_text");
        data.wind_dir = JsonString(payload, "wind_dir");
        data.wind_scale = JsonString(payload, "wind_scale");
        data.humidity = JsonString(payload, "humidity");
        data.update_time = JsonString(payload, "update_time");
        data.air_quality = JsonString(payload, "air_quality");
        data.air_aqi = JsonInt(payload, "air_aqi", -1);
        data.temp_int = JsonInt(payload, "temp");

        const cJSON* forecast = cJSON_GetObjectItem(payload, "forecast");
        const int forecast_count = cJSON_IsArray(forecast) ? std::min(cJSON_GetArraySize(forecast), 7) : 0;
        return;
    }

    if (strcmp(message_type, "news") == 0) {
        return;
    }

    if (strcmp(type->valuestring, "tts") != 0) {
        return;
    }
    const cJSON* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state)) {
        return;
    }

    if (strcmp(state->valuestring, "start") == 0) {
        Schedule([this]() {
            audio_service_.EnableVoiceProcessing(false);
            SetDeviceState(kDeviceStateSpeaking);
        });
    } else if (strcmp(state->valuestring, "stop") == 0) {
        Schedule([this]() {
            SetDeviceState(kDeviceStateIdle);
            resume_listening_.store(conversation_active_.load(std::memory_order_acquire),
                                    std::memory_order_release);
        });
    }
}

void Application::NoteButtonActivity() {
    Board::GetInstance().FlashActivityLed();
    if (silent_boot_refresh_.exchange(false, std::memory_order_acq_rel)) {
        if (silent_boot_fallback_timer_ != nullptr) {
            esp_timer_stop(silent_boot_fallback_timer_);
        }
        ESP_LOGI(kTag, "Button pressed during silent boot: cancelling silent boot mode");
    }
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::ArmSyncSleepTimer() {
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while gallery slideshow is enabled");
        return;
    }

    Settings nvs(kSyncNamespace, false);
    const int interval_minutes = nvs.GetInt(kSyncIntervalKey, 5);
    if (interval_minutes <= 0) {
        ESP_LOGI(kTag, "Sync sleep interval: 关闭");
        return;
    }
    if (sleep_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<Application*>(arg)->EnterScheduledSleep();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "app_sync_sleep";
        ESP_ERROR_CHECK(esp_timer_create(&args, &sleep_timer_));
    }
    esp_timer_stop(sleep_timer_);
    const int64_t delay_us = static_cast<int64_t>(interval_minutes) * 60 * 1000 * 1000;
    ESP_LOGI(kTag, "Sync sleep interval: %d minutes", interval_minutes);
    ESP_LOGI(kTag, "Scheduling sleep after sync interval: %d minutes", interval_minutes);
    ESP_ERROR_CHECK(esp_timer_start_once(sleep_timer_, delay_us));
}

void Application::EnterScheduledSleep() {
    if (conversation_active_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: dialogue page is active");
        ArmSyncSleepTimer();
        return;
    }
    if (IsLocalHttpServiceRunning(rawdraw_ui_manager_.get())) {
        ESP_LOGI(kTag, "Scheduled sleep: stopping local HTTP transfer service");
        rawdraw_ui_manager_->StopLanHttpServer();
        if (rawdraw_ui_manager_->IsHttpServerRunning()) {
            rawdraw_ui_manager_->StopApTransferMode();
        }
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: gallery slideshow is enabled");
        ArmSyncSleepTimer();
        return;
    }

    ESP_LOGI(kTag, "Entering deep sleep after sync interval; BOOT wakes device");
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Entering manual deep sleep; stopping local services and WiFi");
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsHttpServerRunning()) {
        rawdraw_ui_manager_->StopApTransferMode();
    }
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    UpdateStatusBarForUi();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::Run() {
    while (true) {
        std::deque<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(scheduled_mutex_);
            callbacks.swap(scheduled_callbacks_);
        }
        for (auto& callback : callbacks) {
            if (callback) {
                callback();
            }
        }
        if (resume_listening_.load(std::memory_order_acquire) &&
            audio_service_.IsPlaybackIdle() &&
            resume_listening_.exchange(false, std::memory_order_acq_rel)) {
            BeginListening();
        }
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->VoiceWakeupTick();
            rawdraw_ui_manager_->PumpPendingRefreshes();
        }
        // Use longer delay when idle to give tickless idle / light sleep
        // more opportunity. 20ms when there's pending work, 100ms when idle.
        const bool has_pending_work = !callbacks.empty() ||
            resume_listening_.load(std::memory_order_acquire);
        vTaskDelay(pdMS_TO_TICKS(has_pending_work ? 20 : 100));
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (!callback) return;
    std::lock_guard<std::mutex> lock(scheduled_mutex_);
    scheduled_callbacks_.push_back(std::move(callback));
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return true;
}

void Application::UpdateStatusBarForUi() {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running ||
                                protocol_connected_.load(std::memory_order_acquire);
        data.battery_level = battery_level;
        data.battery_charging = charging;
        rawdraw_ui_manager_->UpdateStatusBar(data);
        UpdateWifiSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), wifi_connected);
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        UpdateLanIpSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), lan_ip);
        UpdateHttpServerSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning()
                                         ? lan_ip
                                         : "");
        if (silent_boot_refresh_.load(std::memory_order_acquire)) {
            ESP_LOGD(kTag, "Silent boot active: status bar updated in memory, skipping EPD refresh");
            return;
        }
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return;
}
