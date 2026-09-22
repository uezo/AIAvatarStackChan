#include "AIAvatar.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include "mbedtls/base64.h"

namespace aiavatar {

AIAvatar* AIAvatar::s_instance = nullptr;

AIAvatar::AIAvatar()
    : micMuted_(false),
      serverProcessing_(false),
      wsConnectPending_(false),
      wsDisconnectPending_(false),
      playbackActive_(false),
      pushToTalkActive_(false),
      pttSendPending_(false),
      wsStopPending_(false),
      stackChanHardwareEnabled_(false),
      wifiStarted_(false),
      speakerReady_(false),
      websocketReady_(false),
      openClawReady_(false),
      deferredImagesLogged_(false),
      deferredStartupStage_(0),
      deferredStartupNextMs_(0),
      heavyDeferredResumeMs_(0),
      volume_(200),
      volumeLevelIndex_(0),
      volumeOverlayUntilMs_(0),
      batteryLevel_(-1),
      batteryCharging_(false),
      lastBatteryCheckMs_(0),
      wifiSwitching_(false),
      wifiConnectStarted_(false),
      wifiConnectedLogged_(false),
      timeConfigured_(false),
      pendingWifiIndex_(0),
      wifiSwitchStartMs_(0),
      pttBuf_(nullptr),
      pttBufCapacity_(0),
      pttBufPos_(0),
      pttStartMs_(0),
      pttSendRetryMs_(0),
      visionPreviewJpg_(nullptr),
      visionPreviewJpgLen_(0),
      visionPreviewUntilMs_(0),
      visionPreviewMutex_(nullptr),
      micTaskHandle_(nullptr),
      speakerTaskHandle_(nullptr),
      wsTaskHandle_(nullptr),
      invokeTextQueue_(nullptr),
      visionRequestQueue_(nullptr),
      speechDetectedCb_(nullptr),
      userStartCb_(nullptr),
      userFinalCb_(nullptr),
      userToolCallCb_(nullptr),
      userAcceptedCb_(nullptr),
      userNadeCb_(nullptr),
      userOverlayCb_(nullptr) {}

bool AIAvatar::useStackChan() {
    if (!stackChanHardware_.begin()) return false;
    stackChanHardwareEnabled_ = true;
    motion_.setHardware(&stackChanHardware_);
    leds_.setHardware(&stackChanHardware_);
    return true;
}

bool AIAvatar::begin(const Config& config) {
    ResourceProvider resources;
    resources.useSD(true);
    return begin(config, resources);
}

bool AIAvatar::begin(const Config& config, const ResourceProvider& resources) {
    s_instance = this;
    defaultResources_ = resources;
    config_ = config;
    stackChanHardware_.setNadeMinTouchIntensity(config_.nadeMinTouchIntensity);
    volumeLevelIndex_ = nearestVolumeLevel(config_.speakerVolume);
    volume_ = config_.volumeLevels[volumeLevelIndex_];
    speaker_.setAutoNormalize(config_.audioNormalizeTargetPeak,
                              config_.audioNormalizeMaxGain);
    setenv("TZ", config_.timezone, 1);
    tzset();
    if (!visionPreviewMutex_) {
        visionPreviewMutex_ = xSemaphoreCreateMutex();
    }
    visionRequestQueue_ = xQueueCreate(1, sizeof(InvokeTextMessage));
    if (!visionRequestQueue_) {
        Serial.println("[AIAvatar] vision request queue init failed");
        return false;
    }

    if (config_.fastStartup) {
        bool ok = beginFast();
        if (ok) sleepManager_.begin(*this, config_);
        return ok;
    }
    bool ok = beginNormal();
    if (ok) sleepManager_.begin(*this, config_);
    return ok;
}

bool AIAvatar::beginFast() {
    display_.setResourceProvider(&defaultResources_);
    if (!display_.begin(config_.displayRotation, config_.displayBrightness)) {
        Serial.println("[AIAvatar] display init failed");
    } else if (!face_.beginMinimal(display_)) {
        Serial.println("[AIAvatar] face init failed");
    }
    face_.setDeferredLoadingEnabled(false);
    statusOverlay_.setEnabled(config_.statusOverlayEnabled);
    systemUI_.begin(*this, config_, statusOverlay_);
    leds_.begin(config_);
    stackChanHardware_.setAutoAngleSyncEnabled(config_.stackChanAutoAngleSync);
    motion_.begin(config_.pitchHome);
    motion_.onNade(AIAvatar::onNadeStatic);
    if (stackChanHardwareEnabled_) {
        camera_.begin();
    }
    display_.onOverlay(AIAvatar::drawOverlayStatic);
    updateStatusOverlay();
    display_.update();

    mic_.configure(config_.micSampleRate, config_.micMagnification, config_.micBufferSamples);
    if (!mic_.beginQueue(2)) {
        Serial.println("[AIAvatar] mic queue init failed");
        return false;
    }
    invokeTextQueue_ = xQueueCreate(2, sizeof(InvokeTextMessage));
    if (!invokeTextQueue_) {
        Serial.println("[AIAvatar] invoke text queue init failed");
        return false;
    }
    pttBufCapacity_ = static_cast<size_t>(config_.micSampleRate) * config_.pttMaxSeconds;
    if (pttBufCapacity_ > 0) {
        pttBuf_ = static_cast<int16_t*>(ps_malloc(pttBufCapacity_ * sizeof(int16_t)));
        if (!pttBuf_) pttBuf_ = static_cast<int16_t*>(malloc(pttBufCapacity_ * sizeof(int16_t)));
    }
    if (!pttBuf_) {
        Serial.println("[AIAvatar] PTT buffer allocation failed");
        return false;
    }
    Serial.printf("[AIAvatar] PTT buffer=%u samples %uKB\n",
                  pttBufCapacity_, (pttBufCapacity_ * sizeof(int16_t)) / 1024);
    mic_.begin();
    ws_.setUploadPcmFormat(config_.micSampleRate, 1);
    if (!ws_.reserveInvokeAudioBuffer(pttBufCapacity_)) {
        Serial.println("[AIAvatar] PTT invoke audio buffer allocation failed");
        return false;
    }

    AudioFrameProvider micProvider = {
        AIAvatar::readMicFrameStatic,
        AIAvatar::clearMicFramesStatic,
        this,
    };
    if (!ws_.configureAudioUpload(micProvider, config_.micBufferSamples, config_.micTxSlowBackoffMs,
                                  config_.micTxFailBackoffMs, config_.keepaliveIntervalMs)) {
        Serial.println("[AIAvatar] audio upload init failed");
        return false;
    }

    xTaskCreatePinnedToCore(AIAvatar::micTaskFunc, "AIAvatarMic",
                            config_.audioTaskStackSize, this, 1, &micTaskHandle_,
                            config_.audioTaskCore);
    deferredStartupStage_ = 0;
    deferredStartupNextMs_ = millis() + 50;
    logMemoryUsage("after fast begin");
    return true;
}

bool AIAvatar::beginNormal() {
    beginWiFi();
    wifiStarted_ = true;
    if (!speaker_.begin(config_.playbackQueueDepth, config_.playbackStartThreshold)) {
        return false;
    }
    speaker_.setVolume(volume_);
    speakerReady_ = true;

    display_.setResourceProvider(&defaultResources_);
    if (!display_.begin(config_.displayRotation, config_.displayBrightness)) {
        Serial.println("[AIAvatar] display init failed");
    } else if (!face_.begin(display_)) {
        Serial.println("[AIAvatar] face init failed");
    }
    face_.setDeferredLoadingEnabled(false);
    statusOverlay_.setEnabled(config_.statusOverlayEnabled);
    systemUI_.begin(*this, config_, statusOverlay_);
    leds_.begin(config_);
    stackChanHardware_.setAutoAngleSyncEnabled(config_.stackChanAutoAngleSync);
    motion_.begin(config_.pitchHome);
    motion_.onNade(AIAvatar::onNadeStatic);
    if (stackChanHardwareEnabled_) {
        camera_.begin();
    }
    openClaw_.begin(display_, leds_);
    openClaw_.preload();
    openClawReady_ = true;
    display_.onOverlay(AIAvatar::drawOverlayStatic);

    mic_.configure(config_.micSampleRate, config_.micMagnification, config_.micBufferSamples);
    if (!mic_.beginQueue(2)) {
        Serial.println("[AIAvatar] mic queue init failed");
        return false;
    }
    invokeTextQueue_ = xQueueCreate(2, sizeof(InvokeTextMessage));
    if (!invokeTextQueue_) {
        Serial.println("[AIAvatar] invoke text queue init failed");
        return false;
    }
    pttBufCapacity_ = static_cast<size_t>(config_.micSampleRate) * config_.pttMaxSeconds;
    if (pttBufCapacity_ > 0) {
        pttBuf_ = static_cast<int16_t*>(ps_malloc(pttBufCapacity_ * sizeof(int16_t)));
        if (!pttBuf_) pttBuf_ = static_cast<int16_t*>(malloc(pttBufCapacity_ * sizeof(int16_t)));
    }
    if (!pttBuf_) {
        Serial.println("[AIAvatar] PTT buffer allocation failed");
        return false;
    }
    Serial.printf("[AIAvatar] PTT buffer=%u samples %uKB\n",
                  pttBufCapacity_, (pttBufCapacity_ * sizeof(int16_t)) / 1024);
    mic_.begin();
    ws_.setUploadPcmFormat(config_.micSampleRate, 1);
    if (!ws_.reserveInvokeAudioBuffer(pttBufCapacity_)) {
        Serial.println("[AIAvatar] PTT invoke audio buffer allocation failed");
        return false;
    }

    AudioFrameProvider micProvider = {
        AIAvatar::readMicFrameStatic,
        AIAvatar::clearMicFramesStatic,
        this,
    };
    if (!ws_.configureAudioUpload(micProvider, config_.micBufferSamples, config_.micTxSlowBackoffMs,
                                  config_.micTxFailBackoffMs, config_.keepaliveIntervalMs)) {
        Serial.println("[AIAvatar] audio upload init failed");
        return false;
    }

    ws_.onAudioChunk(AIAvatar::onAudioChunkStatic);
    ws_.onFinal(AIAvatar::onFinalStatic);
    ws_.onFinalText(AIAvatar::onFinalTextStatic);
    ws_.onStop(AIAvatar::onStopStatic);
    ws_.onProcessing(AIAvatar::onProcessingStatic);
    ws_.onStart(AIAvatar::onStartStatic);
    ws_.onToolCall(AIAvatar::onToolCallStatic);
    ws_.onVision(AIAvatar::onVisionStatic);
    ws_.onAccepted(AIAvatar::onAcceptedStatic);
    ws_.begin(config_.wsHost, config_.wsPort, config_.wsPath, config_.userId,
              config_.wsReconnectIntervalMs, config_.channel);
    websocketReady_ = true;

    xTaskCreatePinnedToCore(AIAvatar::micTaskFunc, "AIAvatarMic",
                            config_.audioTaskStackSize, this, 1, &micTaskHandle_,
                            config_.audioTaskCore);
    xTaskCreatePinnedToCore(AIAvatar::speakerTaskFunc, "AIAvatarSpeaker",
                            config_.audioTaskStackSize, this, 1, &speakerTaskHandle_,
                            config_.audioTaskCore);
    xTaskCreatePinnedToCore(AIAvatar::wsTaskFunc, "AIAvatarWS",
                            config_.wsTaskStackSize, this, 1, &wsTaskHandle_,
                            config_.wsTaskCore);
    deferredStartupStage_ = 7;
    logMemoryUsage("after begin");
    return true;
}

void AIAvatar::update() {
    if (config_.fastStartup) updateDeferredStartup();
    sleepManager_.resetWakeActivity();
    if (!motion_.updateHardware()) {
        M5.update();
    }
    updateWiFi();
    systemUI_.update();
    if (config_.fastStartup && deferredStartupStage_ >= 7) {
        face_.setDeferredLoadingEnabled(canRunHeavyDeferredWork());
    }
    face_.update(speaker_.isPlaying(), speaker_.lastChunkRms());
    if (config_.fastStartup && deferredStartupStage_ >= 7 && !deferredImagesLogged_ &&
        face_.deferredLoadingComplete()) {
        deferredImagesLogged_ = true;
        logMemoryUsage("after deferred images");
    }
    motion_.update(playbackActive_);
    leds_.update();
    openClaw_.update();
    if (visualEffects_.update()) {
        display_.setDirty();
    }
    updateVisionPreview();
    updateStatusOverlay();
    display_.update();
    sleepManager_.update();
}

void AIAvatar::updateDeferredStartup() {
    uint32_t now = millis();
    if (static_cast<int32_t>(now - deferredStartupNextMs_) < 0) return;

    switch (deferredStartupStage_) {
        case 0:
            beginDeferredWiFi();
            deferredStartupStage_ = 1;
            deferredStartupNextMs_ = now + 120;
            break;
        case 1:
            if (pushToTalkActive_) {
                deferredStartupNextMs_ = now + 200;
                break;
            }
            face_.loadNextDeferredSprite(true);
            deferredStartupStage_ = 2;
            deferredStartupNextMs_ = now + 120;
            break;
        case 2:
            if (WiFi.status() != WL_CONNECTED) {
                deferredStartupNextMs_ = now + 250;
                break;
            }
            beginDeferredWebSocket();
            deferredStartupStage_ = 3;
            deferredStartupNextMs_ = now + 120;
            break;
        case 3:
            face_.loadNextDeferredSprite(true);
            deferredStartupStage_ = 4;
            deferredStartupNextMs_ = now + 80;
            break;
        case 4:
            face_.loadNextDeferredSprite(true);
            deferredStartupStage_ = 5;
            deferredStartupNextMs_ = now + 80;
            break;
        case 5:
            if (pushToTalkActive_) {
                deferredStartupNextMs_ = now + 200;
                break;
            }
            beginDeferredSpeaker();
            deferredStartupStage_ = 6;
            deferredStartupNextMs_ = now + 120;
            break;
        case 6:
            if (!ws_.isConnected()) {
                deferredStartupNextMs_ = now + 250;
                break;
            }
            if (!canRunHeavyDeferredWork()) {
                deferredStartupNextMs_ = now + 250;
                break;
            }
            beginDeferredOpenClaw();
            face_.setDeferredLoadingEnabled(true);
            deferredStartupStage_ = 7;
            logMemoryUsage("after OpenClaw preload");
            break;
        default:
            break;
    }
}

bool AIAvatar::canRunHeavyDeferredWork() const {
    uint32_t now = millis();
    if (static_cast<int32_t>(now - heavyDeferredResumeMs_) < 0) return false;
    return !pushToTalkActive_ && !pttSendPending_ && !serverProcessing_ && !playbackActive_;
}

void AIAvatar::beginDeferredWiFi() {
    if (wifiStarted_) return;
    wifiStarted_ = true;
    beginWiFi();
}

void AIAvatar::beginDeferredSpeaker() {
    if (speakerReady_) return;
    if (!speaker_.begin(config_.playbackQueueDepth, config_.playbackStartThreshold)) {
        Serial.println("[AIAvatar] speaker init failed");
        return;
    }
    speaker_.setVolume(volume_);
    speakerReady_ = true;
    xTaskCreatePinnedToCore(AIAvatar::speakerTaskFunc, "AIAvatarSpeaker",
                            config_.audioTaskStackSize, this, 1, &speakerTaskHandle_,
                            config_.audioTaskCore);
}

void AIAvatar::beginDeferredWebSocket() {
    if (websocketReady_) return;
    ws_.onAudioChunk(AIAvatar::onAudioChunkStatic);
    ws_.onFinal(AIAvatar::onFinalStatic);
    ws_.onFinalText(AIAvatar::onFinalTextStatic);
    ws_.onStop(AIAvatar::onStopStatic);
    ws_.onProcessing(AIAvatar::onProcessingStatic);
    ws_.onStart(AIAvatar::onStartStatic);
    ws_.onToolCall(AIAvatar::onToolCallStatic);
    ws_.onVision(AIAvatar::onVisionStatic);
    ws_.onAccepted(AIAvatar::onAcceptedStatic);
    ws_.begin(config_.wsHost, config_.wsPort, config_.wsPath, config_.userId,
              config_.wsReconnectIntervalMs, config_.channel);
    websocketReady_ = true;
    xTaskCreatePinnedToCore(AIAvatar::wsTaskFunc, "AIAvatarWS",
                            config_.wsTaskStackSize, this, 1, &wsTaskHandle_,
                            config_.wsTaskCore);
}

void AIAvatar::beginDeferredOpenClaw() {
    if (openClawReady_) return;
    openClaw_.begin(display_, leds_);
    openClaw_.preload();
    openClawReady_ = true;
}

void AIAvatar::setVolume(uint8_t volume) {
    resetSleepTimer("volume");
    volumeLevelIndex_ = nearestVolumeLevel(volume);
    volume_ = config_.volumeLevels[volumeLevelIndex_];
    volumeOverlayUntilMs_ = millis() + 2000;
    speaker_.setVolume(volume_);
    display_.setDirty();
}

void AIAvatar::setVolumeLevel(uint8_t levelIndex) {
    if (config_.volumeLevelCount == 0) return;
    resetSleepTimer("volume");
    if (levelIndex >= config_.volumeLevelCount) levelIndex = config_.volumeLevelCount - 1;
    volumeLevelIndex_ = levelIndex;
    volume_ = config_.volumeLevels[volumeLevelIndex_];
    volumeOverlayUntilMs_ = millis() + 2000;
    speaker_.setVolume(volume_);
    display_.setDirty();
}

void AIAvatar::setMicMuted(bool muted) {
    resetSleepTimer("mic mute");
    micMuted_ = muted;
    display_.setDirty();
}

void AIAvatar::toggleMicMuted() {
    resetSleepTimer("mic mute");
    micMuted_ = !micMuted_;
    display_.setDirty();
}

void AIAvatar::cycleVolume() {
    if (config_.volumeLevelCount == 0) return;
    setVolumeLevel((volumeLevelIndex_ + 1) % config_.volumeLevelCount);
}

bool AIAvatar::startPushToTalk() {
    if (!micMuted_ || !pttBuf_) return false;
    if (config_.fastStartup && pttSendPending_) {
        Serial.println("[AIAvatar] PTT start blocked: send pending");
        return false;
    }
    serverProcessing_ = false;
    if (speakerReady_) speaker_.requestImmediateStop();
    mic_.clearQueue();
    pttBufPos_ = 0;
    pttSendRetryMs_ = 0;
    pttStartMs_ = millis();
    pushToTalkActive_ = true;
    resetSleepTimer("PTT start");
    display_.setDirty();
    Serial.println("[AIAvatar] PTT start");
    return true;
}

void AIAvatar::endPushToTalk() {
    if (!pushToTalkActive_) return;
    resetSleepTimer("PTT end");
    pushToTalkActive_ = false;
    visualEffects_.clearVoiceDetected();

    size_t samples = pttBufPos_;
    size_t minSamples = static_cast<size_t>(config_.pttMinSeconds * config_.micSampleRate);
    if (samples >= minSamples) {
        pttSendPending_ = true;
        Serial.printf("[AIAvatar] PTT end send pending samples=%u\n", samples);
    } else if (samples > 0) {
        Serial.printf("[AIAvatar] PTT discarded samples=%u min=%u\n", samples, minSamples);
    } else {
        Serial.println("[AIAvatar] PTT end no data");
    }
    display_.setDirty();
}

void AIAvatar::setStatusOverlayEnabled(bool enabled) {
    statusOverlay_.setEnabled(enabled);
    display_.setDirty();
}

void AIAvatar::setStackChanAutoAngleSyncEnabled(bool enabled) {
    stackChanHardware_.setAutoAngleSyncEnabled(enabled);
}

void AIAvatar::setOpenClawEffectEnabled(bool enabled) {
    openClaw_.setEnabled(enabled);
}

void AIAvatar::sendStop() {
    resetSleepTimer("stop");
    wsStopPending_ = true;
}

void AIAvatar::connectWebSocket() {
    wsConnectPending_ = true;
}

void AIAvatar::disconnectWebSocket() {
    wsDisconnectPending_ = true;
}

void AIAvatar::switchWiFi(uint8_t networkIndex) {
    if (networkIndex >= config_.wifiNetworkCount) return;
    const auto& network = config_.wifiNetworks[networkIndex];
    if (!network.ssid[0]) return;
    resetSleepTimer("WiFi switch");

    pendingWifiIndex_ = networkIndex;
    wifiSwitching_ = true;
    wifiConnectStarted_ = true;
    wifiConnectedLogged_ = false;
    timeConfigured_ = false;
    wifiSwitchStartMs_ = millis();
    wsConnectPending_ = false;
    wsDisconnectPending_ = true;
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(network.ssid, network.pass);
    Serial.printf("[AIAvatar] switching WiFi to %s\n", network.ssid);
    display_.setDirty();
}

void AIAvatar::micTaskFunc(void* params) {
    static_cast<AIAvatar*>(params)->runMicCapture();
}

void AIAvatar::speakerTaskFunc(void* params) {
    static_cast<AIAvatar*>(params)->runSpeakerPlayback();
}

void AIAvatar::wsTaskFunc(void* params) {
    static_cast<AIAvatar*>(params)->runWebSocket();
}

void AIAvatar::runMicCapture() {
    static int16_t micBuf[kMicBufferSamplesMax];
    uint32_t lastSpeechDetectedMs = 0;

    for (;;) {
        if (playbackActive_) {
            delay(1);
            continue;
        }

        if (mic_.read(micBuf, config_.micBufferSamples)) {
            if (pushToTalkActive_) {
                if (hasSpeech(micBuf, config_.micBufferSamples)) {
                    if (visualEffects_.showVoiceDetected(350)) {
                        display_.setDirty();
                    }
                }
                size_t pos = pttBufPos_;
                if (pos + config_.micBufferSamples <= pttBufCapacity_) {
                    memcpy(pttBuf_ + pos, micBuf, config_.micBufferSamples * sizeof(int16_t));
                    pttBufPos_ = pos + config_.micBufferSamples;
                }
                bool timeout = millis() - pttStartMs_ >= static_cast<uint32_t>(config_.pttMaxSeconds) * 1000;
                bool full = pttBufPos_ + config_.micBufferSamples > pttBufCapacity_;
                if (timeout || full) {
                    pushToTalkActive_ = false;
                    pttSendPending_ = true;
                    Serial.printf("[AIAvatar] PTT auto-end %s samples=%u\n",
                                  timeout ? "timeout" : "full", pttBufPos_);
                    display_.setDirty();
                }
                delay(1);
                continue;
            }

            if (!micMuted_ && ws_.isConnected() && !serverProcessing_) {
                uint32_t now = millis();
                if (hasSpeech(micBuf, config_.micBufferSamples)) {
                    if (visualEffects_.showVoiceDetected(350)) {
                        display_.setDirty();
                    }
                    if (speechDetectedCb_ && now - lastSpeechDetectedMs >= 300) {
                        lastSpeechDetectedMs = now;
                        speechDetectedCb_();
                    }
                }
                mic_.enqueueFrame(micBuf);
            }
        }

        delay(1);
    }
}

void AIAvatar::runSpeakerPlayback() {
    static int16_t playBuf[kPlaybackChunkSamples];
    bool speakerMode = false;
    bool playbackEndSeen = false;
    uint32_t waitStartMs = 0;

    for (;;) {
        if (speaker_.consumeImmediateStopRequested()) {
            speaker_.stopHardware();
            speaker_.resetState();
            mic_.begin();
            playbackActive_ = false;
            speakerMode = false;
            playbackEndSeen = false;
            waitStartMs = 0;
            Serial.println("[AIAvatar] audio -> mic (interrupted)");
        }

        if (!speakerMode && speaker_.hasStartThreshold()) {
            playbackActive_ = true;
            mic_.end();
            speaker_.startHardware();
            speakerMode = true;
            playbackEndSeen = false;
            waitStartMs = 0;
            Serial.println("[AIAvatar] audio -> speaker");
        }

        if (speakerMode) {
            if (!M5.Speaker.isPlaying()) {
                PlaybackEvent event;
                if (speaker_.dequeueEvent(event)) {
                    waitStartMs = 0;
                    switch (event.type) {
                        case PlaybackEventType::Format:
                            speaker_.applyFormat(event.sampleRate, event.channels,
                                                 event.bitsPerSample);
                            break;
                        case PlaybackEventType::PcmFrame:
                            memcpy(playBuf, event.samples, event.sampleCount * sizeof(int16_t));
                            speaker_.releaseFrame(event);
                            speaker_.playFrame(playBuf, event.sampleCount);
                            break;
                        case PlaybackEventType::Face:
                            face_.setExpression(static_cast<Expression>(event.faceId),
                                                event.faceDurationMs);
                            break;
                        case PlaybackEventType::End:
                            playbackEndSeen = true;
                            break;
                        case PlaybackEventType::Stop:
                            playbackEndSeen = true;
                            speaker_.clearQueue();
                            break;
                    }
                } else if (playbackEndSeen) {
                    speaker_.stopHardware();
                    speaker_.resetState();
                    mic_.begin();
                    playbackActive_ = false;
                    speakerMode = false;
                    waitStartMs = 0;
                    Serial.println("[AIAvatar] audio -> mic (ended)");
                } else {
                    if (waitStartMs == 0) waitStartMs = millis();
                    if (millis() - waitStartMs >= config_.playbackDrainTimeoutMs) {
                        speaker_.stopHardware();
                        speaker_.resetState();
                        mic_.begin();
                        playbackActive_ = false;
                        speakerMode = false;
                        waitStartMs = 0;
                        Serial.println("[AIAvatar] audio -> mic (timeout)");
                    }
                }
            }
        }

        delay(1);
    }
}

void AIAvatar::runWebSocket() {
    for (;;) {
        if (wsDisconnectPending_) {
            wsDisconnectPending_ = false;
            ws_.disconnect();
        }
        if (wsConnectPending_) {
            wsConnectPending_ = false;
            ws_.reconnect(config_.wsHost, config_.wsPort, config_.wsPath, config_.userId,
                          config_.wsReconnectIntervalMs, config_.channel);
        }
        if (wsStopPending_) {
            wsStopPending_ = false;
            ws_.sendStop();
        }

        ws_.loop();
        handleInvokeTextSend();
        handlePttSend();
        handleVisionRequest();
        delay(1);
    }
}

void AIAvatar::handleInvokeTextSend() {
    if (!invokeTextQueue_ || !ws_.isConnected()) return;
    InvokeTextMessage msg = {};
    while (xQueueReceive(invokeTextQueue_, &msg, 0) == pdTRUE) {
        bool ok = ws_.sendInvoke(msg.text);
        Serial.printf("[AIAvatar] invoke text %s\n", ok ? "ok" : "failed");
    }
}

void AIAvatar::handlePttSend() {
    if (!pttSendPending_ || !ws_.isConnected()) return;
    if (config_.fastStartup && !speakerReady_) return;
    uint32_t now = millis();
    if (config_.fastStartup && pttSendRetryMs_ != 0 &&
        static_cast<int32_t>(now - pttSendRetryMs_) < 0) {
        return;
    }
    if (!config_.fastStartup) pttSendPending_ = false;
    size_t samples = pttBufPos_;
    if (samples == 0) {
        pttSendPending_ = false;
        return;
    }
    Serial.printf("[AIAvatar] PTT sending samples=%u\n", samples);
    bool ok = ws_.sendInvokeWithAudio(pttBuf_, samples);
    Serial.printf("[AIAvatar] PTT send %s\n", ok ? "ok" : "failed");
    if (!config_.fastStartup) return;
    if (ok) {
        pttSendPending_ = false;
        pttSendRetryMs_ = 0;
        pttBufPos_ = 0;
        heavyDeferredResumeMs_ = now + 10000;
    } else {
        pttSendPending_ = false;
        pttSendRetryMs_ = 0;
        pttBufPos_ = 0;
        heavyDeferredResumeMs_ = now + 1000;
        Serial.println("[AIAvatar] PTT send failed; dropped pending audio");
    }
}

void AIAvatar::handleVisionRequest() {
    if (!visionRequestQueue_ || !ws_.isConnected()) return;
    InvokeTextMessage msg = {};
    if (xQueueReceive(visionRequestQueue_, &msg, 0) != pdTRUE) return;

    if (!camera_.isReady()) {
        Serial.println("[Vision] skipped: camera is not ready");
        return;
    }

    uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;
    if (!camera_.captureJpeg(&jpgBuf, &jpgLen)) {
        return;
    }
    Serial.printf("[Vision] JPEG %u bytes\n", static_cast<unsigned>(jpgLen));
    showVisionPreview(jpgBuf, jpgLen);

    static const char kPrefix[] = "data:image/jpeg;base64,";
    size_t prefixLen = sizeof(kPrefix) - 1;
    size_t b64Len = ((jpgLen + 2) / 3) * 4;
    size_t dataUrlLen = prefixLen + b64Len + 1;

    char* dataUrl = static_cast<char*>(ps_malloc(dataUrlLen));
    if (!dataUrl) dataUrl = static_cast<char*>(malloc(dataUrlLen));
    if (!dataUrl) {
        Serial.printf("[Vision] data URL allocation failed (%u bytes)\n",
                      static_cast<unsigned>(dataUrlLen));
        free(jpgBuf);
        return;
    }

    memcpy(dataUrl, kPrefix, prefixLen);
    size_t actualB64Len = 0;
    int err = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(dataUrl + prefixLen),
                                    b64Len + 1, &actualB64Len,
                                    reinterpret_cast<const unsigned char*>(jpgBuf), jpgLen);
    free(jpgBuf);
    if (err != 0) {
        Serial.printf("[Vision] base64 encode failed: %d\n", err);
        free(dataUrl);
        return;
    }
    dataUrl[prefixLen + actualB64Len] = '\0';

    bool ok = ws_.sendInvokeWithImage(msg.text, dataUrl);
    free(dataUrl);
    Serial.printf("[Vision] invoke %s\n", ok ? "sent" : "failed");
}

bool AIAvatar::invokeText(const char* text) {
    resetSleepTimer("invoke text");
    return queueInvokeText(text);
}

bool AIAvatar::invokeWithVision(const char* text) {
    resetSleepTimer("vision");
    if (!visionRequestQueue_ || !camera_.isReady()) return false;

    InvokeTextMessage msg = {};
    if (strlcpy(msg.text, text ? text : config_.visionInvokePrompt,
                sizeof(msg.text)) >= sizeof(msg.text)) return false;
    if (xQueueSend(visionRequestQueue_, &msg, 0) != pdTRUE) return false;

    leds_.startVisionFlash();
    return true;
}

void AIAvatar::resetSleepTimer(const char* reason) {
    sleepManager_.resetSleepTimer(reason);
}

bool AIAvatar::queueInvokeText(const char* text) {
    if (!invokeTextQueue_ || !text) return false;
    InvokeTextMessage msg = {};
    strlcpy(msg.text, text, sizeof(msg.text));
    if (xQueueSend(invokeTextQueue_, &msg, 0) == pdTRUE) return true;

    InvokeTextMessage dropped = {};
    xQueueReceive(invokeTextQueue_, &dropped, 0);
    return xQueueSend(invokeTextQueue_, &msg, 0) == pdTRUE;
}

void AIAvatar::beginWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    if (config_.wifiSsid[0] == '\0') {
        Serial.println("[AIAvatar] WiFi SSID is empty; starting offline");
        return;
    }

    WiFi.begin(config_.wifiSsid, config_.wifiPass);
    wifiConnectStarted_ = true;
    wifiConnectedLogged_ = false;
    timeConfigured_ = false;
    Serial.printf("[AIAvatar] WiFi connecting: %s\n", config_.wifiSsid);
}

void AIAvatar::updateWiFi() {
    updateWifiSwitch();

    if (!wifiConnectStarted_ || WiFi.status() != WL_CONNECTED) return;

    if (!wifiConnectedLogged_) {
        wifiConnectedLogged_ = true;
        Serial.printf("[AIAvatar] WiFi connected ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    if (!timeConfigured_) {
        timeConfigured_ = true;
        configTzTime(config_.timezone, "ntp.nict.jp", "pool.ntp.org", "time.google.com");
    }
}

void AIAvatar::interruptPlaybackForNewResponse() {
    if (!speakerReady_) return;
    speaker_.requestImmediateStop();
    uint32_t startedAt = millis();
    while (speaker_.immediateStopRequested() && millis() - startedAt < 200) {
        delay(1);
    }
}

void AIAvatar::updateWifiSwitch() {
    if (!wifiSwitching_) return;

    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        const auto& network = config_.wifiNetworks[pendingWifiIndex_];
        strlcpy(config_.wifiSsid, network.ssid, sizeof(config_.wifiSsid));
        strlcpy(config_.wifiPass, network.pass, sizeof(config_.wifiPass));
        wifiSwitching_ = false;
        wsConnectPending_ = true;
        wifiConnectedLogged_ = true;
        Serial.printf("[AIAvatar] WiFi connected to %s ip=%s\n",
                      network.ssid, WiFi.localIP().toString().c_str());
        display_.setDirty();
    } else if (millis() - wifiSwitchStartMs_ >= 15000) {
        wifiSwitching_ = false;
        Serial.println("[AIAvatar] WiFi switch timeout");
        display_.setDirty();
    }
}

void AIAvatar::updateStatusOverlay() {
    uint32_t nowMs = millis();
    if (lastBatteryCheckMs_ == 0 || nowMs - lastBatteryCheckMs_ >= 30000) {
        batteryLevel_ = static_cast<int8_t>(M5.Power.getBatteryLevel());
        batteryCharging_ = M5.Power.isCharging();
        lastBatteryCheckMs_ = nowMs;
    }

    time_t now = time(nullptr);
    struct tm ti = {};
    localtime_r(&now, &ti);

    StatusOverlayState state = {};
    state.micMuted = micMuted_ && !pushToTalkActive_;
    state.volumeVisible = nowMs < volumeOverlayUntilMs_;
    state.volumeLevelCount = config_.volumeLevelCount;
    state.volumeLevel = volumeLevelIndex_;
    state.wifiConnected = WiFi.status() == WL_CONNECTED;
    state.websocketConnected = ws_.isConnected();
    state.batteryLevel = batteryLevel_;
    state.batteryCharging = batteryCharging_;
    state.hour = static_cast<uint8_t>(ti.tm_hour);
    state.minute = static_cast<uint8_t>(ti.tm_min);

    if (statusOverlay_.update(state)) {
        display_.setDirty();
    }
}

void AIAvatar::showVisionPreview(const uint8_t* jpgBuf, size_t jpgLen) {
    if (!jpgBuf || jpgLen == 0 || !display_.ready()) return;

    uint8_t* preview = static_cast<uint8_t*>(ps_malloc(jpgLen));
    if (!preview) preview = static_cast<uint8_t*>(malloc(jpgLen));
    if (!preview) {
        Serial.printf("[Vision] preview allocation failed (%u bytes)\n",
                      static_cast<unsigned>(jpgLen));
        return;
    }
    memcpy(preview, jpgBuf, jpgLen);

    if (visionPreviewMutex_) {
        xSemaphoreTake(visionPreviewMutex_, portMAX_DELAY);
    }
    free(visionPreviewJpg_);
    visionPreviewJpg_ = preview;
    visionPreviewJpgLen_ = jpgLen;
    visionPreviewUntilMs_ = millis() + config_.visionPreviewDurationMs;
    if (visionPreviewMutex_) {
        xSemaphoreGive(visionPreviewMutex_);
    }
    display_.setDirty();
}

void AIAvatar::updateVisionPreview() {
    if (visionPreviewMutex_) {
        xSemaphoreTake(visionPreviewMutex_, portMAX_DELAY);
    }
    bool expired = visionPreviewJpg_ &&
                   static_cast<int32_t>(millis() - visionPreviewUntilMs_) >= 0;
    if (expired) {
        free(visionPreviewJpg_);
        visionPreviewJpg_ = nullptr;
        visionPreviewJpgLen_ = 0;
        visionPreviewUntilMs_ = 0;
    }
    if (visionPreviewMutex_) {
        xSemaphoreGive(visionPreviewMutex_);
    }
    if (expired) display_.setDirty();
}

void AIAvatar::drawVisionPreview(LGFX_Sprite* canvas) {
    if (!canvas) return;

    if (visionPreviewMutex_) {
        xSemaphoreTake(visionPreviewMutex_, portMAX_DELAY);
    }
    bool active = visionPreviewJpg_ && visionPreviewJpgLen_ > 0 &&
                  static_cast<int32_t>(millis() - visionPreviewUntilMs_) < 0;
    if (active) {
        canvas->fillSprite(TFT_BLACK);
        canvas->drawJpg(visionPreviewJpg_, visionPreviewJpgLen_, 0, 0,
                        canvas->width(), canvas->height());
    }
    if (visionPreviewMutex_) {
        xSemaphoreGive(visionPreviewMutex_);
    }
}

void AIAvatar::logMemoryUsage(const char* label) const {
    constexpr uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t internalTotal = heap_caps_get_total_size(kInternalHeapCaps);
    size_t internalFree = heap_caps_get_free_size(kInternalHeapCaps);
    size_t internalMinFree = heap_caps_get_minimum_free_size(kInternalHeapCaps);
    size_t internalLargest = heap_caps_get_largest_free_block(kInternalHeapCaps);

    size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psramMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    size_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    auto usedPct = [](size_t total, size_t freeBytes) -> float {
        return total > 0 ? 100.0f * static_cast<float>(total - freeBytes) /
                               static_cast<float>(total)
                         : 0.0f;
    };

    Serial.printf("[Memory] %s internal used=%uKB/%uKB %.1f%% free=%uKB minFree=%uKB largest=%uKB\n",
                  label ? label : "",
                  static_cast<unsigned>((internalTotal - internalFree) / 1024),
                  static_cast<unsigned>(internalTotal / 1024),
                  usedPct(internalTotal, internalFree),
                  static_cast<unsigned>(internalFree / 1024),
                  static_cast<unsigned>(internalMinFree / 1024),
                  static_cast<unsigned>(internalLargest / 1024));
    Serial.printf("[Memory] %s psram used=%uKB/%uKB %.1f%% free=%uKB minFree=%uKB largest=%uKB\n",
                  label ? label : "",
                  static_cast<unsigned>((psramTotal - psramFree) / 1024),
                  static_cast<unsigned>(psramTotal / 1024),
                  usedPct(psramTotal, psramFree),
                  static_cast<unsigned>(psramFree / 1024),
                  static_cast<unsigned>(psramMinFree / 1024),
                  static_cast<unsigned>(psramLargest / 1024));
}

bool AIAvatar::hasSpeech(const int16_t* samples, size_t sampleCount) const {
    float threshold = 32768.0f * powf(10.0f, config_.vadThresholdDb / 20.0f);
    int64_t thresholdSq = static_cast<int64_t>(threshold * threshold);
    int64_t sum = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
        int32_t sample = samples[i];
        sum += sample * sample;
    }
    return sum > thresholdSq * static_cast<int64_t>(sampleCount);
}

uint8_t AIAvatar::nearestVolumeLevel(uint8_t volume) const {
    if (config_.volumeLevelCount == 0) return 0;
    uint8_t bestIndex = 0;
    uint8_t bestDiff = 255;
    for (uint8_t i = 0; i < config_.volumeLevelCount; ++i) {
        uint8_t level = config_.volumeLevels[i];
        uint8_t diff = volume > level ? volume - level : level - volume;
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIndex = i;
        }
    }
    return bestIndex;
}

bool AIAvatar::readMicFrameStatic(int16_t* dest, void* context) {
    auto* self = static_cast<AIAvatar*>(context);
    return self && self->mic_.dequeueFrame(dest);
}

void AIAvatar::clearMicFramesStatic(void* context) {
    auto* self = static_cast<AIAvatar*>(context);
    if (self) self->mic_.clearQueue();
}

void AIAvatar::onAudioChunkStatic(const IncomingAudioChunk& chunk) {
    if (!s_instance || !s_instance->serverProcessing_) return;
    if (!s_instance->speakerReady_) return;
    SpeakerOutput& speaker = s_instance->speaker_;
    if (chunk.faceName) {
        uint32_t durationMs = chunk.faceDurationSec > 0.0f
                                  ? static_cast<uint32_t>(chunk.faceDurationSec * 1000.0f)
                                  : 0;
        speaker.enqueueFace(static_cast<uint8_t>(FaceController::parseExpression(chunk.faceName)),
                            durationMs);
    }
    if (chunk.pcmData && chunk.pcmSamples > 0) {
        if (chunk.sampleRate > 0) {
            speaker.enqueueFormat(chunk.sampleRate, 1, 16);
        }
        speaker.enqueuePcmFrame(chunk.pcmData, chunk.pcmSamples);
    }
}

void AIAvatar::onFinalStatic() {
    if (!s_instance) return;
    s_instance->resetSleepTimer("final");
    if (s_instance->config_.fastStartup) s_instance->heavyDeferredResumeMs_ = millis() + 500;
    if (s_instance->speakerReady_) s_instance->speaker_.enqueueEnd();
}

void AIAvatar::onFinalTextStatic(const char* responseText, const char* voiceText) {
    if (!s_instance || !s_instance->userFinalCb_) return;
    s_instance->userFinalCb_(responseText, voiceText);
}

void AIAvatar::onStopStatic() {
    if (!s_instance) return;
    s_instance->serverProcessing_ = false;
    if (s_instance->config_.fastStartup) s_instance->heavyDeferredResumeMs_ = millis() + 500;
    if (s_instance->speakerReady_) s_instance->speaker_.enqueueStop();
}

void AIAvatar::onProcessingStatic(bool processing) {
    if (!s_instance) return;
    if (processing) s_instance->resetSleepTimer("processing");
    s_instance->serverProcessing_ = processing;
    if (s_instance->config_.fastStartup && !processing) {
        s_instance->heavyDeferredResumeMs_ = millis() + 500;
    }
}

void AIAvatar::onStartStatic(const char* text) {
    if (!s_instance) return;
    s_instance->resetSleepTimer("speech start");
    s_instance->interruptPlaybackForNewResponse();
    s_instance->openClaw_.handleResponseStart(text);
    if (s_instance->userStartCb_) s_instance->userStartCb_(text);
}

void AIAvatar::onToolCallStatic(const char* toolName) {
    if (!s_instance) return;
    s_instance->resetSleepTimer("tool call");
    if (!s_instance->openClaw_.handleToolCall(toolName)) {
        s_instance->leds_.startToolPulse();
    }
    if (s_instance->userToolCallCb_) s_instance->userToolCallCb_(toolName);
}

void AIAvatar::onVisionStatic() {
    if (s_instance) s_instance->invokeWithVision();
}

void AIAvatar::onAcceptedStatic() {
    if (!s_instance) return;
    s_instance->resetSleepTimer("accepted");
    s_instance->interruptPlaybackForNewResponse();
    s_instance->leds_.startAcceptedFlash();
    if (s_instance->userAcceptedCb_) s_instance->userAcceptedCb_();
}

void AIAvatar::onNadeStatic() {
    if (!s_instance) return;
    s_instance->resetSleepTimer("nade");
    if (s_instance->ws_.isConnected()) {
        struct tm ti;
        time_t now = time(nullptr);
        localtime_r(&now, &ti);

        char invokeBuf[512];
        snprintf(invokeBuf, sizeof(invokeBuf),
                 "%s\n\nCurrent date and time: %04d-%02d-%02d %02d:%02d:%02d",
                 s_instance->config_.nadeInvokePrompt,
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        s_instance->queueInvokeText(invokeBuf);
    }
    if (s_instance->userNadeCb_) s_instance->userNadeCb_();
}

void AIAvatar::drawOverlayStatic(LGFX_Sprite* canvas) {
    if (!s_instance) return;
    s_instance->visualEffects_.draw(canvas);
    if (s_instance->systemUI_.uiVisible()) {
        s_instance->statusOverlay_.draw(canvas);
    }
    s_instance->openClaw_.draw(canvas);
    if (s_instance->userOverlayCb_) {
        s_instance->userOverlayCb_(canvas);
    }
    s_instance->systemUI_.draw(canvas);
    s_instance->drawVisionPreview(canvas);
}

}  // namespace aiavatar
