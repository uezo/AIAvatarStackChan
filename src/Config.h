#pragma once

#include <cstddef>
#include <cstdint>

class Stream;

namespace aiavatar {

static constexpr size_t kMicBufferSamplesMax = 2048;
static constexpr size_t kMicBase64BufferSize =
    ((kMicBufferSamplesMax * sizeof(int16_t) + 2) / 3) * 4 + 1;
static constexpr size_t kPlaybackChunkSamples = 512;
static constexpr uint8_t kMaxWifiNetworks = 5;
static constexpr uint8_t kMaxVolumeLevels = 8;
static constexpr size_t kInvokePromptMaxLen = 512;

enum class SleepWifiMode : uint8_t {
    Sleep,
    Off,
};

struct WifiNetworkConfig {
    char ssid[64];
    char pass[64];
    char name[64];
    SleepWifiMode sleepWifiMode;
    bool sleepWifiModeConfigured;
};

struct RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct Config {
    char wifiSsid[64];
    char wifiPass[64];
    char wsHost[128];
    uint16_t wsPort;
    char wsPath[128];
    char userId[64];
    char channel[32];
    char timezone[48];
    WifiNetworkConfig wifiNetworks[kMaxWifiNetworks];
    uint8_t wifiNetworkCount;

    uint32_t micSampleRate;
    uint8_t micMagnification;
    size_t micBufferSamples;
    int8_t vadThresholdDb;

    size_t playbackQueueDepth;
    size_t playbackStartThreshold;
    uint32_t playbackDrainTimeoutMs;
    uint8_t speakerVolume;
    float audioNormalizeTargetPeak;
    float audioNormalizeMaxGain;
    uint8_t volumeLevels[kMaxVolumeLevels];
    uint8_t volumeLevelCount;

    size_t audioTaskStackSize;
    int audioTaskCore;
    size_t wsTaskStackSize;
    int wsTaskCore;
    uint32_t wsReconnectIntervalMs;
    uint32_t micTxSlowBackoffMs;
    uint32_t micTxFailBackoffMs;
    uint32_t keepaliveIntervalMs;

    uint8_t displayRotation;
    uint8_t displayBrightness;
    bool sleepEnabled;
    uint32_t sleepTimeoutMs;
    uint8_t sleepDisplayBrightness;
    SleepWifiMode sleepWifiMode;
    bool statusOverlayEnabled;
    uint32_t visionPreviewDurationMs;
    RgbColor acceptedLedColor;
    RgbColor toolLedColor;
    uint8_t pttMaxSeconds;
    float pttMinSeconds;
    uint32_t pttHoldThresholdMs;
    int16_t pitchHome;
    bool stackChanAutoAngleSync;
    uint8_t nadeMinTouchIntensity;
    char nadeInvokePrompt[kInvokePromptMaxLen];
    char visionInvokePrompt[kInvokePromptMaxLen];

    bool fastStartup;
    bool debugLog;

    Config();
    bool loadFromJson(Stream& stream);
    bool loadFromJsonBytes(const uint8_t* data, size_t len);
    bool loadFromSD(const char* path = "/config.json");
};

}  // namespace aiavatar
