#include "Config.h"
#include "ResourceProvider.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <strings.h>

namespace aiavatar {

namespace {

void loadRgbColor(JsonDocument& doc, const char* key, RgbColor& color) {
    if (!doc[key].is<JsonArray>()) return;
    JsonArray arr = doc[key].as<JsonArray>();
    if (arr.size() < 3) return;
    color.r = arr[0].as<uint8_t>();
    color.g = arr[1].as<uint8_t>();
    color.b = arr[2].as<uint8_t>();
}

SleepWifiMode parseSleepWifiMode(const char* value, SleepWifiMode fallback) {
    if (!value || !value[0]) return fallback;
    if (strcasecmp(value, "off") == 0 || strcasecmp(value, "disconnect") == 0) {
        return SleepWifiMode::Off;
    }
    if (strcasecmp(value, "sleep") == 0 || strcasecmp(value, "modem_sleep") == 0 ||
        strcasecmp(value, "keep") == 0) {
        return SleepWifiMode::Sleep;
    }
    return fallback;
}

}  // namespace

Config::Config()
    : wsPort(443),
      wifiNetworkCount(0),
      micSampleRate(16000),
      micMagnification(16),
      micBufferSamples(1024),
      vadThresholdDb(-40),
      playbackQueueDepth(2048),
      playbackStartThreshold(kPlaybackChunkSamples * 2),
      playbackDrainTimeoutMs(500),
      speakerVolume(80),
      audioNormalizeTargetPeak(0.0f),
      audioNormalizeMaxGain(8.0f),
      volumeLevelCount(5),
      audioTaskStackSize(8192),
      audioTaskCore(0),
      wsTaskStackSize(8192),
      wsTaskCore(1),
      wsReconnectIntervalMs(5000),
      micTxSlowBackoffMs(500),
      micTxFailBackoffMs(3000),
      keepaliveIntervalMs(1000),
      displayRotation(1),
      displayBrightness(128),
      sleepEnabled(false),
      sleepTimeoutMs(10UL * 60UL * 1000UL),
      sleepDisplayBrightness(0),
      sleepWifiMode(SleepWifiMode::Sleep),
      statusOverlayEnabled(true),
      visionPreviewDurationMs(2000),
      acceptedLedColor{0, 168, 0},
      toolLedColor{140, 0, 140},
      pttMaxSeconds(30),
      pttMinSeconds(0.2f),
      pttHoldThresholdMs(200),
      pitchHome(200),
      stackChanAutoAngleSync(true),
      nadeMinTouchIntensity(2),
      fastStartup(false),
      debugLog(false) {
    wifiSsid[0] = '\0';
    wifiPass[0] = '\0';
    wsHost[0] = '\0';
    strlcpy(wsPath, "/ws", sizeof(wsPath));
    strlcpy(userId, "default", sizeof(userId));
    channel[0] = '\0';
    strlcpy(timezone, "JST-9", sizeof(timezone));
    strlcpy(nadeInvokePrompt,
            "$The user gently patted your head. React with one very short phrase. "
            "If they pat you too often in a short time, you may sound a little annoyed or shy. "
            "When you are upset or troubled, you may use angry or sorrow as the face.",
            sizeof(nadeInvokePrompt));
    strlcpy(visionInvokePrompt,
            "$I am providing visual context needed for your reply. Do not merely describe what is "
            "visible; respond appropriately as if you saw this image in the current conversation context.",
            sizeof(visionInvokePrompt));
    const uint8_t defaultVolumeLevels[] = {0, 32, 80, 160, 255};
    memcpy(volumeLevels, defaultVolumeLevels, sizeof(defaultVolumeLevels));
    for (uint8_t i = sizeof(defaultVolumeLevels); i < kMaxVolumeLevels; ++i) {
        volumeLevels[i] = 0;
    }
    for (auto& network : wifiNetworks) {
        network.ssid[0] = '\0';
        network.pass[0] = '\0';
        network.name[0] = '\0';
        network.sleepWifiMode = SleepWifiMode::Sleep;
        network.sleepWifiModeConfigured = false;
    }
}

static bool applyJsonDocument(Config& config, JsonDocument& doc) {
    strlcpy(config.wifiSsid, doc["wifi_ssid"] | config.wifiSsid, sizeof(config.wifiSsid));
    strlcpy(config.wifiPass, doc["wifi_pass"] | config.wifiPass, sizeof(config.wifiPass));
    if (doc["wifi_networks"].is<JsonArray>()) {
        JsonArray networks = doc["wifi_networks"].as<JsonArray>();
        config.wifiNetworkCount = 0;
        for (JsonObject network : networks) {
            if (config.wifiNetworkCount >= kMaxWifiNetworks) break;
            const char* ssid = network["ssid"] | "";
            if (!ssid[0]) continue;
            auto& dst = config.wifiNetworks[config.wifiNetworkCount++];
            strlcpy(dst.ssid, ssid, sizeof(dst.ssid));
            strlcpy(dst.pass, network["pass"] | "", sizeof(dst.pass));
            strlcpy(dst.name, network["name"] | "", sizeof(dst.name));
            const char* sleepWifiMode = network["sleep_wifi_mode"] | "";
            dst.sleepWifiModeConfigured = sleepWifiMode[0] != '\0';
            dst.sleepWifiMode = parseSleepWifiMode(sleepWifiMode, config.sleepWifiMode);
        }
        if (config.wifiSsid[0] == '\0' && config.wifiNetworkCount > 0) {
            strlcpy(config.wifiSsid, config.wifiNetworks[0].ssid, sizeof(config.wifiSsid));
            strlcpy(config.wifiPass, config.wifiNetworks[0].pass, sizeof(config.wifiPass));
        }
    }
    if (config.wifiSsid[0] != '\0' && config.wifiNetworkCount == 0) {
        strlcpy(config.wifiNetworks[0].ssid, config.wifiSsid, sizeof(config.wifiNetworks[0].ssid));
        strlcpy(config.wifiNetworks[0].pass, config.wifiPass, sizeof(config.wifiNetworks[0].pass));
        config.wifiNetworks[0].name[0] = '\0';
        config.wifiNetworks[0].sleepWifiMode = config.sleepWifiMode;
        config.wifiNetworks[0].sleepWifiModeConfigured = false;
        config.wifiNetworkCount = 1;
    }
    strlcpy(config.wsHost, doc["ws_host"] | config.wsHost, sizeof(config.wsHost));
    config.wsPort = doc["ws_port"] | config.wsPort;
    strlcpy(config.wsPath, doc["ws_path"] | config.wsPath, sizeof(config.wsPath));
    strlcpy(config.userId, doc["user_id"] | config.userId, sizeof(config.userId));
    strlcpy(config.channel, doc["channel"] | config.channel, sizeof(config.channel));
    strlcpy(config.timezone, doc["timezone"] | config.timezone, sizeof(config.timezone));

    config.micSampleRate = doc["mic_sample_rate"] | config.micSampleRate;
    config.micMagnification = doc["mic_magnification"] | config.micMagnification;
    size_t requestedMicSamples = doc["mic_buffer_samples"] | config.micBufferSamples;
    if (requestedMicSamples > 0 && requestedMicSamples <= kMicBufferSamplesMax) {
        config.micBufferSamples = requestedMicSamples;
    }
    config.vadThresholdDb = doc["vad_threshold_db"] | config.vadThresholdDb;

    if (doc.containsKey("playback_queue_depth")) {
        config.playbackQueueDepth = doc["playback_queue_depth"].as<size_t>();
    } else if (doc.containsKey("rbuf_samples")) {
        size_t legacySamples = doc["rbuf_samples"].as<size_t>();
        config.playbackQueueDepth = (legacySamples + kPlaybackChunkSamples - 1) / kPlaybackChunkSamples;
        if (config.playbackQueueDepth > 2048) config.playbackQueueDepth = 2048;
    }
    config.playbackStartThreshold = doc["start_threshold"] | config.playbackStartThreshold;
    config.playbackDrainTimeoutMs = doc["drain_timeout_ms"] | config.playbackDrainTimeoutMs;
    config.speakerVolume = doc["speaker_volume"] | config.speakerVolume;
    config.audioNormalizeTargetPeak =
        doc["audio_normalize_target_peak"] | config.audioNormalizeTargetPeak;
    config.audioNormalizeMaxGain =
        doc["audio_normalize_max_gain"] | config.audioNormalizeMaxGain;
    if (config.audioNormalizeTargetPeak < 0.0f) config.audioNormalizeTargetPeak = 0.0f;
    if (config.audioNormalizeTargetPeak > 1.0f) config.audioNormalizeTargetPeak = 1.0f;
    if (config.audioNormalizeMaxGain < 1.0f) config.audioNormalizeMaxGain = 1.0f;
    if (doc["volume_levels"].is<JsonArray>()) {
        JsonArray levels = doc["volume_levels"].as<JsonArray>();
        uint8_t count = 0;
        for (JsonVariant level : levels) {
            if (count >= kMaxVolumeLevels) break;
            config.volumeLevels[count++] = level.as<uint8_t>();
        }
        if (count >= 2) {
            config.volumeLevelCount = count;
        }
    }

    config.audioTaskStackSize = doc["audio_task_stack_size"] | config.audioTaskStackSize;
    config.audioTaskCore = doc["audio_task_core"] | config.audioTaskCore;
    config.wsTaskStackSize = doc["ws_task_stack_size"] | config.wsTaskStackSize;
    config.wsTaskCore = doc["ws_task_core"] | config.wsTaskCore;
    config.wsReconnectIntervalMs = doc["ws_reconnect_interval_ms"] | config.wsReconnectIntervalMs;
    config.micTxSlowBackoffMs = doc["mic_tx_slow_backoff_ms"] | config.micTxSlowBackoffMs;
    config.micTxFailBackoffMs = doc["mic_tx_fail_backoff_ms"] | config.micTxFailBackoffMs;
    config.keepaliveIntervalMs = doc["keepalive_interval_ms"] | config.keepaliveIntervalMs;
    config.displayRotation = doc["display_rotation"] | config.displayRotation;
    config.displayBrightness = doc["display_brightness"] | config.displayBrightness;
    config.sleepEnabled = doc["sleep_enabled"] | config.sleepEnabled;
    config.sleepTimeoutMs = doc["sleep_timeout_ms"] | config.sleepTimeoutMs;
    config.sleepDisplayBrightness =
        doc["sleep_display_brightness"] | config.sleepDisplayBrightness;
    config.sleepWifiMode =
        parseSleepWifiMode(doc["sleep_wifi_mode"] | "", config.sleepWifiMode);
    config.statusOverlayEnabled = doc["status_overlay_enabled"] | config.statusOverlayEnabled;
    config.visionPreviewDurationMs = doc["vision_preview_duration_ms"] | config.visionPreviewDurationMs;
    loadRgbColor(doc, "accepted_led_color", config.acceptedLedColor);
    loadRgbColor(doc, "tool_led_color", config.toolLedColor);
    config.pttMaxSeconds = doc["ptt_max_seconds"] | config.pttMaxSeconds;
    config.pttMinSeconds = doc["ptt_min_seconds"] | config.pttMinSeconds;
    config.pttHoldThresholdMs = doc["ptt_hold_threshold_ms"] | config.pttHoldThresholdMs;
    config.pitchHome = doc["pitch_home"] | config.pitchHome;
    config.stackChanAutoAngleSync = doc["stackchan_auto_angle_sync"] | config.stackChanAutoAngleSync;
    if (doc["nade_min_touch_intensity"].is<uint8_t>()) {
        uint8_t intensity = doc["nade_min_touch_intensity"].as<uint8_t>();
        if (intensity >= 1 && intensity <= 3) config.nadeMinTouchIntensity = intensity;
    }
    strlcpy(config.nadeInvokePrompt, doc["nade_invoke_prompt"] | config.nadeInvokePrompt,
            sizeof(config.nadeInvokePrompt));
    strlcpy(config.visionInvokePrompt, doc["vision_invoke_prompt"] | config.visionInvokePrompt,
            sizeof(config.visionInvokePrompt));
    config.fastStartup = doc["fast_startup"] | config.fastStartup;
    config.debugLog = doc["debug_log"] | config.debugLog;

    Serial.printf("[Config] WS: %s:%u%s user=%s\n",
                  config.wsHost, config.wsPort, config.wsPath, config.userId);
    Serial.printf("[Config] mic=%uHz/%u samples speakerVol=%u normalizePeak=%.2f maxGain=%.1f\n",
                  config.micSampleRate, config.micBufferSamples, config.speakerVolume,
                  config.audioNormalizeTargetPeak, config.audioNormalizeMaxGain);
    return true;
}

bool Config::loadFromJson(Stream& stream) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, stream);
    if (err) {
        Serial.printf("[Config] JSON parse error: %s\n", err.c_str());
        return false;
    }
    return applyJsonDocument(*this, doc);
}

bool Config::loadFromJsonBytes(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.printf("[Config] JSON parse error: %s\n", err.c_str());
        return false;
    }
    return applyJsonDocument(*this, doc);
}

bool Config::loadFromSD(const char* path) {
    ResourceProvider resources;
    resources.useSD(true);
    return resources.loadConfig(*this, path);
}

}  // namespace aiavatar
