#pragma once

#include "Config.h"

#include <M5Unified.h>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace aiavatar {

enum class PlaybackEventType : uint8_t {
    Format,
    PcmFrame,
    Face,
    End,
    Stop,
};

struct PlaybackEvent {
    PlaybackEventType type;
    uint32_t sampleRate;
    uint8_t channels;
    uint8_t bitsPerSample;
    size_t sampleCount;
    int16_t* samples;
    int16_t frameIndex;
    uint8_t faceId;
    uint32_t faceDurationMs;
};

class SpeakerOutput {
public:
    SpeakerOutput();

    bool begin(size_t queueDepth, size_t startThreshold);
    void startHardware();
    void stopHardware();
    void requestImmediateStop();
    void setVolume(uint8_t volume);
    void setPcmGain(float gain) { pcmGain_ = gain > 0.0f ? gain : 1.0f; }
    float pcmGain() const { return pcmGain_; }
    void setCodecDacVolume(uint8_t value) { codecDacVolume_ = value; }
    void setAutoNormalize(float targetPeak, float maxGain = 8.0f);
    void setAudioStatsLogEnabled(bool enabled) { audioStatsLogEnabled_ = enabled; }

    bool enqueueFormat(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);
    bool enqueuePcmFrame(const int16_t* samples, size_t sampleCount);
    bool enqueueFace(uint8_t faceId, uint32_t durationMs);
    bool enqueueEnd();
    bool enqueueStop();

    bool dequeueEvent(PlaybackEvent& event);
    void clearQueue();

    bool hasStartThreshold() const;
    bool isPlaying() const { return playing_; }
    bool endReceived() const { return endReceived_; }
    bool immediateStopRequested() const { return immediateStopRequested_; }
    bool consumeImmediateStopRequested();
    size_t queuedSamples() const { return queuedSamples_; }
    uint32_t sampleRate() const { return sampleRate_; }
    float lastChunkRms() const { return lastChunkRms_; }
    float lastChunkPeak() const { return lastChunkPeak_; }

    bool playFrame(const int16_t* samples, size_t sampleCount);
    void applyFormat(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);
    void resetState();
    void releaseFrame(const PlaybackEvent& event);

private:
    QueueHandle_t queue_;
    uint8_t* queueStorage_;
    StaticQueue_t queueControl_;
    SemaphoreHandle_t stateMutex_;
    int16_t** frames_;
    bool* frameUsed_;
    size_t queueDepth_;
    size_t nextFrameIndex_;
    size_t startThreshold_;
    volatile size_t queuedSamples_;
    bool gotFormat_;
    bool pendingFormat_;
    bool endReceived_;
    bool hardwareStarted_;
    bool playing_;
    volatile bool immediateStopRequested_;
    volatile uint8_t volume_;
    float pcmGain_;
    float normalizeTargetPeak_;
    float normalizeMaxGain_;
    float lastPlaybackGain_;
    uint8_t codecDacVolume_;
    uint32_t sampleRate_;
    uint8_t channels_;
    uint8_t bitsPerSample_;
    uint32_t pendingSampleRate_;
    uint8_t pendingChannels_;
    uint8_t pendingBitsPerSample_;
    float lastChunkRms_;
    float lastChunkPeak_;
    size_t lastChunkClippedSamples_;
    uint32_t playbackFrameCount_;
    uint32_t lastAudioStatsLogMs_;
    bool audioStatsLogEnabled_;

    bool lockState(uint32_t timeoutMs = portMAX_DELAY);
    void unlockState();
    int16_t* acquireFrame(int16_t& frameIndex);
    int16_t* acquireFrameLocked(int16_t& frameIndex);
    bool enqueueEvent(const PlaybackEvent& event);
    bool enqueueEventLocked(const PlaybackEvent& event);
    void releaseFrameLocked(const PlaybackEvent& event);
    void applyCodecDacVolume();
    void updateRms(const int16_t* samples, size_t sampleCount);
    float computePlaybackGain() const;
    void logAudioStats(size_t sampleCount);
};

}  // namespace aiavatar
