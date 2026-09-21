#include "SpeakerOutput.h"

#include <Arduino.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace aiavatar {

SpeakerOutput::SpeakerOutput()
    : queue_(nullptr),
      queueStorage_(nullptr),
      stateMutex_(nullptr),
      frames_(nullptr),
      frameUsed_(nullptr),
      queueDepth_(0),
      nextFrameIndex_(0),
      startThreshold_(kPlaybackChunkSamples * 2),
      queuedSamples_(0),
      gotFormat_(false),
      pendingFormat_(false),
      endReceived_(false),
      hardwareStarted_(false),
      playing_(false),
      immediateStopRequested_(false),
      volume_(200),
      pcmGain_(1.0f),
      normalizeTargetPeak_(0.0f),
      normalizeMaxGain_(8.0f),
      lastPlaybackGain_(1.0f),
      codecDacVolume_(0),
      sampleRate_(16000),
      channels_(1),
      bitsPerSample_(16),
      pendingSampleRate_(16000),
      pendingChannels_(1),
      pendingBitsPerSample_(16),
      lastChunkRms_(0.0f),
      lastChunkPeak_(0.0f),
      lastChunkClippedSamples_(0),
      playbackFrameCount_(0),
      lastAudioStatsLogMs_(0),
      audioStatsLogEnabled_(false) {}

bool SpeakerOutput::begin(size_t queueDepth, size_t startThreshold) {
    queueDepth_ = queueDepth > 0 ? queueDepth : 64;
    startThreshold_ = startThreshold > 0 ? startThreshold : kPlaybackChunkSamples * 2;

    const size_t frameBytes = kPlaybackChunkSamples * sizeof(int16_t);
    Serial.printf("[Speaker] allocating playback frames: requested=%u frameBytes=%u freeHeap=%u psram=%u\n",
                  queueDepth_, frameBytes, ESP.getFreeHeap(), ESP.getFreePsram());

    const size_t freePsram = ESP.getFreePsram();
    const bool psramAvailable = freePsram > frameBytes * 16;
    if (!psramAvailable) {
        size_t heapSafeDepth = (ESP.getFreeHeap() > 120000)
                                   ? (ESP.getFreeHeap() - 120000) / frameBytes
                                   : 0;
        if (heapSafeDepth < queueDepth_) {
            queueDepth_ = heapSafeDepth;
            Serial.printf("[Speaker] PSRAM unavailable, limiting heap playback depth to %u\n",
                          queueDepth_);
        }
    }
    if (queueDepth_ < 16) {
        Serial.println("[Speaker] playback frame pool allocation failed: no PSRAM and not enough heap");
        return false;
    }

    stateMutex_ = xSemaphoreCreateRecursiveMutex();
    if (!stateMutex_) {
        Serial.println("[Speaker] playback state mutex allocation failed");
        return false;
    }

    frames_ = static_cast<int16_t**>(calloc(queueDepth_, sizeof(int16_t*)));
    frameUsed_ = static_cast<bool*>(calloc(queueDepth_, sizeof(bool)));
    if (!frames_ || !frameUsed_) {
        Serial.println("[Speaker] playback frame table allocation failed");
        return false;
    }

    size_t allocated = 0;
    for (; allocated < queueDepth_; ++allocated) {
        if (psramAvailable) {
            frames_[allocated] = static_cast<int16_t*>(ps_malloc(frameBytes));
        } else {
            frames_[allocated] = static_cast<int16_t*>(malloc(frameBytes));
        }
        if (!frames_[allocated]) {
            break;
        }
    }

    if (allocated < 16) {
        Serial.printf("[Speaker] playback frame pool allocation failed allocated=%u\n", allocated);
        return false;
    }

    queueDepth_ = allocated;
    bool queueInPsram = false;
    if (psramAvailable) {
        size_t queueBytes = queueDepth_ * sizeof(PlaybackEvent);
        queueStorage_ = static_cast<uint8_t*>(ps_malloc(queueBytes));
        if (queueStorage_) {
            queue_ = xQueueCreateStatic(queueDepth_, sizeof(PlaybackEvent),
                                        queueStorage_, &queueControl_);
            queueInPsram = queue_ != nullptr;
            if (!queue_) {
                free(queueStorage_);
                queueStorage_ = nullptr;
            }
        }
    }
    if (!queue_) {
        queue_ = xQueueCreate(queueDepth_, sizeof(PlaybackEvent));
    }
    if (!queue_) {
        Serial.println("[Speaker] playback queue allocation failed");
        return false;
    }

    Serial.printf("[Speaker] queue=%u events threshold=%u samples capacity=%.2fs "
                  "frames=%s queueStorage=%s\n",
                  queueDepth_, startThreshold_,
                  (queueDepth_ * kPlaybackChunkSamples) / 16000.0f,
                  psramAvailable ? "psram" : "heap",
                  queueInPsram ? "psram" : "internal");
    return true;
}

void SpeakerOutput::startHardware() {
    if (hardwareStarted_) return;
    M5.Speaker.begin();
    hardwareStarted_ = true;
    M5.Speaker.setVolume(volume_);
    auto spkcfg = M5.Speaker.config();
    Serial.printf("[Speaker] M5 speaker cfg mck=%d bck=%d ws=%d dout=%d port=%d stereo=%d mag=%u rate=%u vol=%u\n",
                  spkcfg.pin_mck, spkcfg.pin_bck, spkcfg.pin_ws, spkcfg.pin_data_out,
                  static_cast<int>(spkcfg.i2s_port), spkcfg.stereo, spkcfg.magnification,
                  spkcfg.sample_rate, M5.Speaker.getVolume());
    applyCodecDacVolume();
    playing_ = true;
}

void SpeakerOutput::stopHardware() {
    if (!hardwareStarted_) return;
    while (M5.Speaker.isPlaying()) delay(1);
    M5.Speaker.end();
    hardwareStarted_ = false;
    playing_ = false;
}

void SpeakerOutput::requestImmediateStop() {
    immediateStopRequested_ = true;
    if (hardwareStarted_) {
        M5.Speaker.stop();
    }
    clearQueue();
}

void SpeakerOutput::setVolume(uint8_t volume) {
    volume_ = volume;
    if (hardwareStarted_) M5.Speaker.setVolume(volume_);
}

void SpeakerOutput::setAutoNormalize(float targetPeak, float maxGain) {
    if (targetPeak < 0.0f) targetPeak = 0.0f;
    if (targetPeak > 1.0f) targetPeak = 1.0f;
    if (maxGain < 1.0f) maxGain = 1.0f;
    normalizeTargetPeak_ = targetPeak;
    normalizeMaxGain_ = maxGain;
    Serial.printf("[Speaker] auto normalize targetPeak=%.2f maxGain=%.1f\n",
                  normalizeTargetPeak_, normalizeMaxGain_);
}

void SpeakerOutput::applyCodecDacVolume() {
    if (codecDacVolume_ == 0) return;

    static constexpr uint8_t kEs8311Address = 0x18;
    static constexpr uint8_t kEs8311DacVolumeRegister = 0x32;
    bool ok = M5.In_I2C.writeRegister8(kEs8311Address, kEs8311DacVolumeRegister,
                                       codecDacVolume_, 100000);
    Serial.printf("[Speaker] ES8311 DAC volume reg 0x32=0x%02X %s\n",
                  codecDacVolume_, ok ? "ok" : "failed");
}

bool SpeakerOutput::enqueueFormat(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
    uint32_t effectiveRate = sampleRate > 0 ? sampleRate : 16000;
    uint8_t effectiveChannels = channels > 0 ? channels : 1;
    uint8_t effectiveBits = bitsPerSample > 0 ? bitsPerSample : 16;

    if (!lockState(100)) {
        Serial.println("[Speaker] playback state lock timeout on enqueue format");
        return false;
    }
    if (gotFormat_ && sampleRate_ == effectiveRate && channels_ == effectiveChannels &&
        bitsPerSample_ == effectiveBits) {
        unlockState();
        return true;
    }
    if (pendingFormat_ && pendingSampleRate_ == effectiveRate &&
        pendingChannels_ == effectiveChannels && pendingBitsPerSample_ == effectiveBits) {
        unlockState();
        return true;
    }

    PlaybackEvent event = {};
    event.type = PlaybackEventType::Format;
    event.sampleRate = effectiveRate;
    event.channels = effectiveChannels;
    event.bitsPerSample = effectiveBits;
    bool queued = enqueueEventLocked(event);
    if (queued) {
        pendingFormat_ = true;
        pendingSampleRate_ = effectiveRate;
        pendingChannels_ = effectiveChannels;
        pendingBitsPerSample_ = effectiveBits;
    }
    unlockState();
    return queued;
}

bool SpeakerOutput::enqueuePcmFrame(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) return false;

    size_t offset = 0;
    bool ok = true;
    while (offset < sampleCount) {
        int16_t frameIndex = -1;
        if (!lockState(2000)) {
            Serial.println("[Speaker] playback state lock timeout");
            return false;
        }

        int16_t* frame = acquireFrameLocked(frameIndex);
        if (!frame) {
            unlockState();
            uint32_t waitStartMs = millis();
            do {
                delay(1);
                if (!lockState(10)) continue;
                frame = acquireFrameLocked(frameIndex);
                if (frame) break;
                unlockState();
            } while (millis() - waitStartMs < 2000);

            if (!frame) {
                static uint32_t lastPoolFullLogMs = 0;
                if (millis() - lastPoolFullLogMs >= 1000) {
                    lastPoolFullLogMs = millis();
                    Serial.printf("[Speaker] playback pool full queued=%u capacity=%.2fs\n",
                                  queuedSamples_,
                                  (queueDepth_ * kPlaybackChunkSamples) / (float)sampleRate_);
                }
                return false;
            }
        }

        PlaybackEvent event = {};
        event.type = PlaybackEventType::PcmFrame;
        event.sampleCount = min(sampleCount - offset, kPlaybackChunkSamples);
        event.samples = frame;
        event.frameIndex = frameIndex;
        memcpy(frame, samples + offset, event.sampleCount * sizeof(int16_t));
        ok = enqueueEventLocked(event) && ok;
        unlockState();
        offset += event.sampleCount;
    }
    return ok;
}

bool SpeakerOutput::enqueueFace(uint8_t faceId, uint32_t durationMs) {
    PlaybackEvent event = {};
    event.type = PlaybackEventType::Face;
    event.faceId = faceId;
    event.faceDurationMs = durationMs;
    return enqueueEvent(event);
}

bool SpeakerOutput::enqueueEnd() {
    PlaybackEvent event = {};
    event.type = PlaybackEventType::End;
    if (!lockState(2000)) {
        Serial.println("[Speaker] playback state lock timeout on end");
        return false;
    }
    endReceived_ = true;
    bool queued = enqueueEventLocked(event);
    unlockState();
    return queued;
}

bool SpeakerOutput::enqueueStop() {
    PlaybackEvent event = {};
    event.type = PlaybackEventType::Stop;
    if (!lockState(2000)) {
        Serial.println("[Speaker] playback state lock timeout on stop");
        return false;
    }
    endReceived_ = true;
    bool queued = enqueueEventLocked(event);
    unlockState();
    return queued;
}

bool SpeakerOutput::dequeueEvent(PlaybackEvent& event) {
    if (!queue_) return false;
    if (xQueueReceive(queue_, &event, 0) != pdTRUE) return false;
    if (!lockState(100)) return true;
    if (event.type == PlaybackEventType::PcmFrame && queuedSamples_ >= event.sampleCount) {
        queuedSamples_ -= event.sampleCount;
    }
    unlockState();
    return true;
}

bool SpeakerOutput::consumeImmediateStopRequested() {
    if (!immediateStopRequested_) return false;
    immediateStopRequested_ = false;
    return true;
}

void SpeakerOutput::clearQueue() {
    if (!lockState(2000)) {
        Serial.println("[Speaker] playback state lock timeout on clear");
        return;
    }
    if (queue_) xQueueReset(queue_);
    if (frameUsed_) memset(frameUsed_, 0, queueDepth_ * sizeof(bool));
    queuedSamples_ = 0;
    pendingFormat_ = false;
    endReceived_ = false;
    lastChunkRms_ = 0.0f;
    lastChunkPeak_ = 0.0f;
    lastChunkClippedSamples_ = 0;
    playbackFrameCount_ = 0;
    lastAudioStatsLogMs_ = 0;
    lastPlaybackGain_ = 1.0f;
    unlockState();
}

bool SpeakerOutput::hasStartThreshold() const {
    return queuedSamples_ >= startThreshold_ || (endReceived_ && queuedSamples_ > 0);
}

bool SpeakerOutput::playFrame(const int16_t* samples, size_t sampleCount) {
    if (!hardwareStarted_ || !samples || sampleCount == 0 || M5.Speaker.isPlaying()) return false;
    int16_t amplified[kPlaybackChunkSamples];
    const int16_t* playSamples = samples;
    updateRms(samples, sampleCount);

    float playbackGain = computePlaybackGain();
    lastPlaybackGain_ = playbackGain;
    if (playbackGain != 1.0f) {
        size_t n = sampleCount > kPlaybackChunkSamples ? kPlaybackChunkSamples : sampleCount;
        for (size_t i = 0; i < n; ++i) {
            float v = samples[i] * playbackGain;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            amplified[i] = static_cast<int16_t>(v);
        }
        playSamples = amplified;
        sampleCount = n;
    }
    logAudioStats(sampleCount);
    M5.Speaker.playRaw(playSamples, sampleCount, sampleRate_, false, 1);
    while (M5.Speaker.isPlaying()) {
        if (immediateStopRequested_) {
            M5.Speaker.stop();
        }
        delay(1);
    }
    return !immediateStopRequested_;
}

void SpeakerOutput::applyFormat(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
    if (!lockState(100)) {
        Serial.println("[Speaker] playback state lock timeout on format");
        return;
    }
    sampleRate_ = sampleRate > 0 ? sampleRate : 16000;
    channels_ = channels > 0 ? channels : 1;
    bitsPerSample_ = bitsPerSample > 0 ? bitsPerSample : 16;
    playbackFrameCount_ = 0;
    lastAudioStatsLogMs_ = 0;
    lastPlaybackGain_ = 1.0f;
    gotFormat_ = true;
    pendingFormat_ = false;
    Serial.printf("[Speaker] format=%uHz %ubit %uch\n", sampleRate_, bitsPerSample_, channels_);
    unlockState();
}

void SpeakerOutput::resetState() {
    clearQueue();
    if (!lockState(100)) return;
    gotFormat_ = false;
    pendingFormat_ = false;
    unlockState();
}

bool SpeakerOutput::lockState(uint32_t timeoutMs) {
    if (!stateMutex_) return false;
    TickType_t ticks = timeoutMs == portMAX_DELAY
                           ? portMAX_DELAY
                           : pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTakeRecursive(stateMutex_, ticks) == pdTRUE;
}

void SpeakerOutput::unlockState() {
    if (stateMutex_) xSemaphoreGiveRecursive(stateMutex_);
}

void SpeakerOutput::releaseFrame(const PlaybackEvent& event) {
    if (!lockState(100)) return;
    releaseFrameLocked(event);
    unlockState();
}

void SpeakerOutput::releaseFrameLocked(const PlaybackEvent& event) {
    if (event.type != PlaybackEventType::PcmFrame) return;
    if (!frameUsed_ || event.frameIndex < 0 || static_cast<size_t>(event.frameIndex) >= queueDepth_) {
        return;
    }
    frameUsed_[event.frameIndex] = false;
}

int16_t* SpeakerOutput::acquireFrame(int16_t& frameIndex) {
    if (!lockState(100)) return nullptr;
    int16_t* frame = acquireFrameLocked(frameIndex);
    unlockState();
    return frame;
}

int16_t* SpeakerOutput::acquireFrameLocked(int16_t& frameIndex) {
    if (!frames_ || !frameUsed_ || queueDepth_ == 0) return nullptr;
    for (size_t i = 0; i < queueDepth_; ++i) {
        size_t idx = (nextFrameIndex_ + i) % queueDepth_;
        if (!frameUsed_[idx]) {
            frameUsed_[idx] = true;
            nextFrameIndex_ = (idx + 1) % queueDepth_;
            frameIndex = static_cast<int16_t>(idx);
            return frames_[idx];
        }
    }
    return nullptr;
}

bool SpeakerOutput::enqueueEvent(const PlaybackEvent& event) {
    if (!queue_) return false;
    if (!lockState(2000)) {
        Serial.println("[Speaker] playback state lock timeout on enqueue");
        return false;
    }
    bool queued = enqueueEventLocked(event);
    unlockState();
    return queued;
}

bool SpeakerOutput::enqueueEventLocked(const PlaybackEvent& event) {
    if (!queue_) return false;
    bool queued = xQueueSend(queue_, &event, 0) == pdTRUE;
    if (!queued) {
        uint32_t waitStartMs = millis();
        while (millis() - waitStartMs < 2000) {
            delay(1);
            if (xQueueSend(queue_, &event, 0) == pdTRUE) {
                queued = true;
                break;
            }
        }
        if (!queued) {
            static uint32_t lastQueueFullLogMs = 0;
            if (millis() - lastQueueFullLogMs >= 1000) {
                lastQueueFullLogMs = millis();
                Serial.printf("[Speaker] playback queue full type=%u queued=%u\n",
                              static_cast<uint8_t>(event.type), queuedSamples_);
            }
            releaseFrameLocked(event);
            return false;
        }
    }

    if (event.type == PlaybackEventType::PcmFrame) {
        queuedSamples_ += event.sampleCount;
    }
    return true;
}

void SpeakerOutput::updateRms(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        lastChunkRms_ = 0.0f;
        lastChunkPeak_ = 0.0f;
        lastChunkClippedSamples_ = 0;
        return;
    }
    float sum = 0.0f;
    int32_t peak = 0;
    size_t clipped = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
        int32_t sample = samples[i];
        int32_t absSample = sample < 0 ? -sample : sample;
        if (absSample > peak) peak = absSample;
        if (absSample >= 32760) ++clipped;
        float normalized = samples[i] / 32768.0f;
        sum += normalized * normalized;
    }
    lastChunkRms_ = sqrtf(sum / sampleCount);
    lastChunkPeak_ = peak / 32768.0f;
    lastChunkClippedSamples_ = clipped;
}

float SpeakerOutput::computePlaybackGain() const {
    float gain = pcmGain_;
    if (normalizeTargetPeak_ > 0.0f && lastChunkPeak_ > 0.0f) {
        float normalizeGain = normalizeTargetPeak_ / lastChunkPeak_;
        if (normalizeGain > normalizeMaxGain_) normalizeGain = normalizeMaxGain_;
        if (normalizeGain < 1.0f) normalizeGain = 1.0f;
        gain *= normalizeGain;
    }
    return gain;
}

void SpeakerOutput::logAudioStats(size_t sampleCount) {
    if (!audioStatsLogEnabled_) return;

    ++playbackFrameCount_;

    uint32_t now = millis();
    bool shouldLog = playbackFrameCount_ <= 4 ||
                     lastAudioStatsLogMs_ == 0 ||
                     now - lastAudioStatsLogMs_ >= 1000;
    if (!shouldLog) return;
    lastAudioStatsLogMs_ = now;

    float rmsDb = lastChunkRms_ > 0.0f ? 20.0f * log10f(lastChunkRms_) : -120.0f;
    float peakDb = lastChunkPeak_ > 0.0f ? 20.0f * log10f(lastChunkPeak_) : -120.0f;
    float outPeak = lastChunkPeak_ * lastPlaybackGain_;
    if (outPeak > 1.0f) outPeak = 1.0f;
    float outPeakDb = outPeak > 0.0f ? 20.0f * log10f(outPeak) : -120.0f;
    float gainDb = lastPlaybackGain_ > 0.0f ? 20.0f * log10f(lastPlaybackGain_) : 0.0f;
    Serial.printf("[Speaker] pcm stats frame=%u samples=%u rate=%u peak=%.3f/%+.1fdBFS rms=%.3f/%+.1fdBFS clipped=%u gain=%.2f/%+.1fdB outPeak=%.3f/%+.1fdBFS\n",
                  playbackFrameCount_, sampleCount, sampleRate_,
                  lastChunkPeak_, peakDb, lastChunkRms_, rmsDb,
                  lastChunkClippedSamples_, lastPlaybackGain_, gainDb,
                  outPeak, outPeakDb);
}

}  // namespace aiavatar
