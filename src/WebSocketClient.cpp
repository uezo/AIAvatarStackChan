#include "WebSocketClient.h"

#include "Config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "mbedtls/base64.h"

namespace aiavatar {

static WebSocketClient* s_wsInstance = nullptr;

static bool useTlsForPort(uint16_t port) {
    return port == 443;
}

static void beginWebSocket(WebSocketsClient& ws, const char* host, uint16_t port, const char* path) {
    if (useTlsForPort(port)) {
        ws.beginSSL(host, port, path);
    } else {
        ws.begin(host, port, path);
    }
}

WebSocketClient::WebSocketClient()
    : connected_(false),
      autoReconnectEnabled_(true),
      connectionCb_(nullptr),
      audioChunkCb_(nullptr),
      startCb_(nullptr),
      stopCb_(nullptr),
      finalCb_(nullptr),
      finalTextCb_(nullptr),
      acceptedCb_(nullptr),
      serverSpeechDetectedCb_(nullptr),
      processingCb_(nullptr),
      faceCb_(nullptr),
      toolCallCb_(nullptr),
      visionCb_(nullptr),
      audioTxBuf_(nullptr),
      audioTxEncodedBuf_(nullptr),
      audioTxEncodedCapacity_(0),
      uploadConverter_(nullptr),
      playbackConverter_(nullptr),
      uploadPcmSampleRate_(16000),
      uploadPcmChannels_(1),
      audioRxRawBuf_(nullptr),
      audioRxRawCapacity_(0),
      audioRxPcmBuf_(nullptr),
      audioRxPcmCapacity_(0),
      audioFrameProvider_{nullptr, nullptr, nullptr},
      audioTxFrameSamples_(0),
      audioTxSlowBackoffMs_(500),
      audioTxFailBackoffMs_(3000),
      keepaliveIntervalMs_(1000),
      audioTxResumeMs_(0),
      lastAudioSendMs_(0),
      invokeAudioBuf_(nullptr),
      invokeAudioBufCapacity_(0) {
    sessionId_[0] = '\0';
    userId_[0] = '\0';
    channel_[0] = '\0';
}

bool WebSocketClient::configureAudioUpload(const AudioFrameProvider& provider,
                                           size_t frameSamples,
                                           uint32_t slowBackoffMs,
                                           uint32_t failBackoffMs,
                                           uint32_t keepaliveIntervalMs) {
    if (!provider.read || frameSamples == 0) return false;

    audioFrameProvider_ = provider;
    audioTxFrameSamples_ = frameSamples;
    audioTxSlowBackoffMs_ = slowBackoffMs;
    audioTxFailBackoffMs_ = failBackoffMs;
    keepaliveIntervalMs_ = keepaliveIntervalMs;

    if (!audioTxBuf_) {
        audioTxBuf_ = static_cast<int16_t*>(malloc(audioTxFrameSamples_ * sizeof(int16_t)));
        if (!audioTxBuf_) {
            Serial.println("[WS] audio tx buffer allocation failed");
            return false;
        }
    }
    return true;
}

bool WebSocketClient::reserveInvokeAudioBuffer(size_t sampleCount) {
    if (sampleCount == 0) return false;

    size_t rawBytes = sampleCount * sizeof(int16_t);
    if (uploadConverter_) {
        size_t encodedCapacity = uploadConverter_->maxEncodedBytes(sampleCount, uploadPcmChannels_);
        if (encodedCapacity == 0) return false;
        rawBytes = encodedCapacity;
    }

    size_t b64Len = ((rawBytes + 2) / 3) * 4;
    static constexpr size_t kJsonOverhead = 480;
    size_t required = kJsonOverhead + b64Len + 1;
    if (invokeAudioBuf_ && invokeAudioBufCapacity_ >= required) return true;

    free(invokeAudioBuf_);
    invokeAudioBuf_ = static_cast<char*>(ps_malloc(required));
    if (!invokeAudioBuf_) invokeAudioBuf_ = static_cast<char*>(malloc(required));
    invokeAudioBufCapacity_ = invokeAudioBuf_ ? required : 0;
    if (!invokeAudioBuf_) {
        Serial.printf("[WS] invoke audio reserve failed (%u bytes)\n",
                      static_cast<unsigned>(required));
        return false;
    }

    Serial.printf("[WS] invoke audio buffer=%uKB for %u samples\n",
                  static_cast<unsigned>(required / 1024),
                  static_cast<unsigned>(sampleCount));
    return true;
}

void WebSocketClient::setUploadPcmFormat(uint32_t sampleRate, uint8_t channels) {
    uploadPcmSampleRate_ = sampleRate > 0 ? sampleRate : 16000;
    uploadPcmChannels_ = channels > 0 ? channels : 1;
}

void WebSocketClient::begin(const char* host, uint16_t port, const char* path,
                            const char* userId, uint32_t reconnectIntervalMs,
                            const char* channel) {
    s_wsInstance = this;
    autoReconnectEnabled_ = true;
    strlcpy(userId_, userId ? userId : "", sizeof(userId_));
    strlcpy(channel_, channel ? channel : "", sizeof(channel_));
    Serial.printf("[WS] begin %s://%s:%u%s\n", useTlsForPort(port) ? "wss" : "ws", host, port, path);
    beginWebSocket(ws_, host, port, path);
    ws_.onEvent(WebSocketClient::onEventStatic);
    ws_.setReconnectInterval(reconnectIntervalMs);
}

void WebSocketClient::reconnect(const char* host, uint16_t port, const char* path,
                                const char* userId, uint32_t reconnectIntervalMs,
                                const char* channel) {
    autoReconnectEnabled_ = true;
    strlcpy(userId_, userId ? userId : "", sizeof(userId_));
    strlcpy(channel_, channel ? channel : "", sizeof(channel_));
    ws_.disconnect();
    delay(100);
    Serial.printf("[WS] reconnect %s://%s:%u%s\n", useTlsForPort(port) ? "wss" : "ws", host, port, path);
    beginWebSocket(ws_, host, port, path);
    ws_.onEvent(WebSocketClient::onEventStatic);
    ws_.setReconnectInterval(reconnectIntervalMs);
}

void WebSocketClient::disconnect() {
    autoReconnectEnabled_ = false;
    ws_.disconnect();
}

void WebSocketClient::loop() {
    if (!autoReconnectEnabled_ && !connected_) return;
    pumpAudioUpload();
    ws_.loop();
    if (!pumpAudioUpload()) {
        pumpKeepalive();
    }
}

void WebSocketClient::generateSessionId() {
    uint8_t rnd[16];
    for (int i = 0; i < 16; ++i) rnd[i] = esp_random() & 0xff;
    rnd[6] = (rnd[6] & 0x0f) | 0x40;
    rnd[8] = (rnd[8] & 0x3f) | 0x80;
    snprintf(sessionId_, sizeof(sessionId_),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
             rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
}

void WebSocketClient::sendStart() {
    JsonDocument doc;
    doc["type"] = "start";
    doc["session_id"] = sessionId_;
    doc["user_id"] = userId_;
    if (channel_[0]) doc["channel"] = channel_;
    String out;
    serializeJson(doc, out);
    ws_.sendTXT(out);
}

bool WebSocketClient::sendAudioData(const int16_t* pcmData, size_t sampleCount,
                                    uint32_t* elapsedMs) {
    if (!connected_ || !pcmData || sampleCount == 0) return false;

    const uint8_t* rawData = reinterpret_cast<const uint8_t*>(pcmData);
    size_t rawBytes = sampleCount * sizeof(int16_t);
    AudioFormat format = {"pcm16", uploadPcmSampleRate_, uploadPcmChannels_, 16};
    bool converted = false;
    if (uploadConverter_) {
        size_t encodedCapacity = uploadConverter_->maxEncodedBytes(sampleCount, uploadPcmChannels_);
        if (encodedCapacity == 0) return false;
        if (audioTxEncodedCapacity_ < encodedCapacity) {
            free(audioTxEncodedBuf_);
            audioTxEncodedBuf_ = static_cast<uint8_t*>(malloc(encodedCapacity));
            audioTxEncodedCapacity_ = audioTxEncodedBuf_ ? encodedCapacity : 0;
        }
        if (!audioTxEncodedBuf_) return false;
        size_t encodedLen = 0;
        if (!uploadConverter_->encode(pcmData, sampleCount, uploadPcmChannels_,
                                      audioTxEncodedBuf_, audioTxEncodedCapacity_, encodedLen)) {
            return false;
        }
        format = uploadConverter_->encodedFormat(uploadPcmSampleRate_, uploadPcmChannels_);
        rawData = audioTxEncodedBuf_;
        rawBytes = encodedLen;
        converted = true;
    }

    const size_t b64Size = ((rawBytes + 2) / 3) * 4 + 1;
    static char b64[kMicBase64BufferSize];
    if (b64Size > sizeof(b64)) return false;

    size_t b64Len = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), sizeof(b64), &b64Len,
                              rawData, rawBytes) != 0) {
        return false;
    }
    b64[b64Len] = '\0';

    String msg;
    msg.reserve(b64Len + (converted ? 220 : 96));
    msg = "{\"type\":\"data\",\"session_id\":\"";
    msg += sessionId_;
    msg += "\",\"audio_data\":\"";
    msg += b64;
    if (converted) {
        msg += "\",\"metadata\":{\"audio_format\":{\"codec\":\"";
        msg += format.codec;
        msg += "\",\"sample_rate\":";
        msg += format.sampleRate;
        msg += ",\"channels\":";
        msg += format.channels;
        msg += ",\"bits_per_sample\":";
        msg += format.bitsPerSample;
        msg += "}}}";
    } else {
        msg += "\"}";
    }

    uint32_t startMs = millis();
    bool ok = ws_.sendTXT(msg);
    if (elapsedMs) *elapsedMs = millis() - startMs;
    return ok;
}

bool WebSocketClient::pumpAudioUpload() {
    if (!connected_ || !audioFrameProvider_.read || !audioTxBuf_ || audioTxFrameSamples_ == 0) {
        return false;
    }

    uint32_t now = millis();
    if (now < audioTxResumeMs_) return false;
    if (!audioFrameProvider_.read(audioTxBuf_, audioFrameProvider_.context)) return false;

    uint32_t sendMs = 0;
    bool ok = sendAudioData(audioTxBuf_, audioTxFrameSamples_, &sendMs);
    lastAudioSendMs_ = now;
    if (!ok || sendMs >= 1000) {
        audioTxResumeMs_ = now + audioTxFailBackoffMs_;
        if (audioFrameProvider_.clear) audioFrameProvider_.clear(audioFrameProvider_.context);
    } else if (sendMs >= 250) {
        audioTxResumeMs_ = now + audioTxSlowBackoffMs_;
        if (audioFrameProvider_.clear) audioFrameProvider_.clear(audioFrameProvider_.context);
    }
    return ok;
}

void WebSocketClient::pumpKeepalive() {
    static const int16_t silenceBuf[16] = {};
    uint32_t now = millis();
    if (!connected_ || now < audioTxResumeMs_ || keepaliveIntervalMs_ == 0) return;
    if (now - lastAudioSendMs_ < keepaliveIntervalMs_) return;
    if (sendAudioData(silenceBuf, 16)) {
        lastAudioSendMs_ = now;
    }
}

bool WebSocketClient::ensureRxRawCapacity(size_t bytes) {
    if (audioRxRawCapacity_ >= bytes) return true;
    free(audioRxRawBuf_);
    audioRxRawBuf_ = static_cast<uint8_t*>(ps_malloc(bytes));
    if (!audioRxRawBuf_) audioRxRawBuf_ = static_cast<uint8_t*>(malloc(bytes));
    audioRxRawCapacity_ = audioRxRawBuf_ ? bytes : 0;
    return audioRxRawBuf_ != nullptr;
}

bool WebSocketClient::ensureRxPcmCapacity(size_t samples) {
    if (audioRxPcmCapacity_ >= samples) return true;
    free(audioRxPcmBuf_);
    audioRxPcmBuf_ = static_cast<int16_t*>(ps_malloc(samples * sizeof(int16_t)));
    if (!audioRxPcmBuf_) audioRxPcmBuf_ = static_cast<int16_t*>(malloc(samples * sizeof(int16_t)));
    audioRxPcmCapacity_ = audioRxPcmBuf_ ? samples : 0;
    return audioRxPcmBuf_ != nullptr;
}

size_t WebSocketClient::downmixToMono(const int16_t* input, size_t sampleCount,
                                      uint8_t channels) {
    if (!input || channels == 0) return 0;
    if (channels == 1) {
        if (input != audioRxPcmBuf_) {
            if (!ensureRxPcmCapacity(sampleCount)) return 0;
            memcpy(audioRxPcmBuf_, input, sampleCount * sizeof(int16_t));
        }
        return sampleCount;
    }

    size_t monoSamples = sampleCount / channels;
    if (!ensureRxPcmCapacity(monoSamples)) return 0;
    for (size_t i = 0; i < monoSamples; ++i) {
        int32_t sum = 0;
        for (uint8_t ch = 0; ch < channels; ++ch) {
            sum += input[i * channels + ch];
        }
        audioRxPcmBuf_[i] = static_cast<int16_t>(sum / channels);
    }
    return monoSamples;
}

bool WebSocketClient::decodeIncomingAudio(const char* base64Data, size_t base64Len,
                                          const AudioFormat& wireFormat,
                                          IncomingAudioChunk& chunk) {
    if (!base64Data || base64Len == 0) return false;

    size_t rawCapacity = ((base64Len + 3) / 4) * 3;
    if (!ensureRxRawCapacity(rawCapacity)) {
        Serial.printf("[WS] audio rx raw allocation failed (%u bytes)\n",
                      static_cast<unsigned>(rawCapacity));
        return false;
    }

    size_t rawLen = 0;
    if (mbedtls_base64_decode(audioRxRawBuf_, audioRxRawCapacity_, &rawLen,
                              reinterpret_cast<const unsigned char*>(base64Data),
                              base64Len) != 0) {
        Serial.println("[WS] audio base64 decode failed");
        return false;
    }

    const bool encodedAudio = wireFormat.codec && wireFormat.codec[0] != '\0' &&
                              strcasecmp(wireFormat.codec, "pcm16") != 0;
    if (encodedAudio) {
        if (!playbackConverter_) {
            Serial.printf("[WS] codec=%s but playback converter is not set\n", wireFormat.codec);
            return false;
        }
        size_t requiredSamples = playbackConverter_->maxDecodedSamples(rawLen, wireFormat);
        if (requiredSamples == 0 || !ensureRxPcmCapacity(requiredSamples)) {
            Serial.printf("[WS] audio rx pcm allocation failed (%u samples)\n",
                          static_cast<unsigned>(requiredSamples));
            return false;
        }
        size_t decodedSamples = 0;
        if (!playbackConverter_->decode(audioRxRawBuf_, rawLen, wireFormat, audioRxPcmBuf_,
                                        audioRxPcmCapacity_, decodedSamples)) {
            Serial.printf("[WS] codec decode failed: %s\n", wireFormat.codec);
            return false;
        }
        size_t monoSamples = downmixToMono(audioRxPcmBuf_, decodedSamples, wireFormat.channels);
        if (monoSamples == 0) return false;
        chunk.pcmData = audioRxPcmBuf_;
        chunk.pcmSamples = monoSamples;
        chunk.channels = 1;
        chunk.bitsPerSample = 16;
        return true;
    }

    if (wireFormat.bitsPerSample != 0 && wireFormat.bitsPerSample != 16) {
        Serial.printf("[WS] unsupported PCM bits=%u\n", wireFormat.bitsPerSample);
        return false;
    }
    size_t sampleCount = rawLen / sizeof(int16_t);
    auto* pcm = reinterpret_cast<const int16_t*>(audioRxRawBuf_);
    uint8_t channels = wireFormat.channels > 0 ? wireFormat.channels : 1;
    if (channels == 1) {
        chunk.pcmData = pcm;
        chunk.pcmSamples = sampleCount;
        chunk.channels = 1;
        chunk.bitsPerSample = 16;
        return true;
    }

    size_t monoSamples = downmixToMono(pcm, sampleCount, channels);
    if (monoSamples == 0) return false;
    chunk.pcmData = audioRxPcmBuf_;
    chunk.pcmSamples = monoSamples;
    chunk.channels = 1;
    chunk.bitsPerSample = 16;
    return true;
}

bool WebSocketClient::sendInvoke(const char* text) {
    if (!connected_) return false;
    JsonDocument doc;
    doc["type"] = "invoke";
    doc["session_id"] = sessionId_;
    doc["user_id"] = userId_;
    if (channel_[0]) doc["channel"] = channel_;
    doc["text"] = text ? text : "";
    doc["allow_merge"] = false;
    doc["wait_in_queue"] = true;
    String out;
    serializeJson(doc, out);
    return ws_.sendTXT(out);
}

bool WebSocketClient::sendInvokeWithImage(const char* text, const char* imageDataUrl) {
    if (!connected_) return false;

    JsonDocument doc;
    doc["type"] = "invoke";
    doc["session_id"] = sessionId_;
    doc["user_id"] = userId_;
    if (channel_[0]) doc["channel"] = channel_;
    doc["text"] = text ? text : "";
    // Borrow the image buffer for this call instead of duplicating it in the document.
    doc["files"][0]["url"] = JsonString(imageDataUrl ? imageDataUrl : "", true);
    doc["allow_merge"] = false;
    doc["wait_in_queue"] = true;
    if (doc.overflowed()) return false;

    size_t bufSize = measureJson(doc) + 1;
    char* buf = static_cast<char*>(ps_malloc(bufSize));
    if (!buf) buf = static_cast<char*>(malloc(bufSize));
    if (!buf) {
        Serial.printf("[WS] invoke image allocation failed (%u bytes)\n", bufSize);
        return false;
    }

    size_t written = serializeJson(doc, buf, bufSize);
    if (written == 0 || written >= bufSize) {
        free(buf);
        return false;
    }

    bool ok = ws_.sendTXT(buf, written);
    free(buf);
    Serial.printf("[WS] invoke image bytes=%u ok=%d\n", static_cast<unsigned>(written), ok ? 1 : 0);
    return ok;
}

bool WebSocketClient::sendInvokeWithAudio(const int16_t* pcmData, size_t sampleCount) {
    if (!connected_ || !pcmData || sampleCount == 0) return false;

    const uint8_t* rawData = reinterpret_cast<const uint8_t*>(pcmData);
    size_t rawBytes = sampleCount * sizeof(int16_t);
    AudioFormat format = {"pcm16", uploadPcmSampleRate_, uploadPcmChannels_, 16};
    uint8_t* convertedBuf = nullptr;
    bool converted = false;
    if (uploadConverter_) {
        size_t encodedCapacity = uploadConverter_->maxEncodedBytes(sampleCount, uploadPcmChannels_);
        if (encodedCapacity == 0) return false;
        convertedBuf = static_cast<uint8_t*>(ps_malloc(encodedCapacity));
        if (!convertedBuf) convertedBuf = static_cast<uint8_t*>(malloc(encodedCapacity));
        if (!convertedBuf) {
            Serial.printf("[WS] invoke audio convert buffer allocation failed (%u bytes)\n",
                          static_cast<unsigned>(encodedCapacity));
            return false;
        }
        size_t encodedLen = 0;
        if (!uploadConverter_->encode(pcmData, sampleCount, uploadPcmChannels_, convertedBuf,
                                      encodedCapacity, encodedLen)) {
            free(convertedBuf);
            return false;
        }
        format = uploadConverter_->encodedFormat(uploadPcmSampleRate_, uploadPcmChannels_);
        rawData = convertedBuf;
        rawBytes = encodedLen;
        converted = true;
    }

    size_t b64Len = ((rawBytes + 2) / 3) * 4;
    static constexpr size_t kJsonOverhead = 480;
    size_t bufSize = kJsonOverhead + b64Len + 1;
    char channelField[64] = "";
    if (channel_[0]) {
        snprintf(channelField, sizeof(channelField), "\"channel\":\"%s\",", channel_);
    }
    if (!invokeAudioBuf_ || invokeAudioBufCapacity_ < bufSize) {
        if (!reserveInvokeAudioBuffer(sampleCount)) {
            Serial.printf("[WS] invoke audio allocation failed (%u bytes)\n",
                          static_cast<unsigned>(bufSize));
            free(convertedBuf);
            return false;
        }
    }
    char* buf = invokeAudioBuf_;
    if (invokeAudioBufCapacity_ < bufSize) {
        Serial.printf("[WS] invoke audio buffer too small required=%u capacity=%u\n",
                      static_cast<unsigned>(bufSize),
                      static_cast<unsigned>(invokeAudioBufCapacity_));
        free(convertedBuf);
        return false;
    }

    int headerLen = snprintf(buf, kJsonOverhead,
                             "{\"type\":\"invoke\",\"session_id\":\"%s\","
                             "\"user_id\":\"%s\","
                             "%s"
                             "\"text\":\"\","
                             "\"audio_data\":\"",
                             sessionId_, userId_, channelField);
    if (headerLen <= 0 || static_cast<size_t>(headerLen) >= kJsonOverhead) {
        free(convertedBuf);
        return false;
    }

    size_t actualB64Len = 0;
    int err = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(buf + headerLen),
                                    bufSize - headerLen, &actualB64Len,
                                    rawData, rawBytes);
    if (err != 0) {
        free(convertedBuf);
        return false;
    }

    int footerLen = 0;
    if (converted) {
        footerLen = snprintf(buf + headerLen + actualB64Len,
                             bufSize - headerLen - actualB64Len,
                             "\",\"metadata\":{\"audio_format\":{\"codec\":\"%s\","
                             "\"sample_rate\":%u,\"channels\":%u,\"bits_per_sample\":%u}},"
                             "\"allow_merge\":false,\"wait_in_queue\":true}",
                             format.codec, format.sampleRate, format.channels,
                             format.bitsPerSample);
    } else {
        footerLen = snprintf(buf + headerLen + actualB64Len,
                             bufSize - headerLen - actualB64Len,
                             "\",\"allow_merge\":false,\"wait_in_queue\":true}");
    }
    if (footerLen <= 0) {
        free(convertedBuf);
        return false;
    }

    size_t totalLen = headerLen + actualB64Len + footerLen;
    bool ok = ws_.sendTXT(buf, totalLen);
    free(convertedBuf);
    Serial.printf("[WS] invoke audio samples=%u b64=%uKB ok=%d\n",
                  sampleCount, static_cast<uint32_t>(actualB64Len / 1024), ok ? 1 : 0);
    return ok;
}

void WebSocketClient::sendStop() {
    if (!connected_) return;
    JsonDocument doc;
    doc["type"] = "stop";
    doc["session_id"] = sessionId_;
    String out;
    serializeJson(doc, out);
    ws_.sendTXT(out);
    if (stopCb_) stopCb_();
}

void WebSocketClient::onEventStatic(WStype_t type, uint8_t* payload, size_t length) {
    if (s_wsInstance) s_wsInstance->onEvent(type, payload, length);
}

void WebSocketClient::onEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            connected_ = true;
            generateSessionId();
            sendStart();
            Serial.printf("[WS] connected session=%s\n", sessionId_);
            if (connectionCb_) connectionCb_(true);
            break;

        case WStype_DISCONNECTED:
            connected_ = false;
            Serial.println("[WS] disconnected");
            if (processingCb_) processingCb_(false);
            if (stopCb_) stopCb_();
            if (connectionCb_) connectionCb_(false);
            break;

        case WStype_TEXT: {
            pumpAudioUpload();

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload, length);
            if (err) {
                Serial.printf("[WS] JSON parse error: %s\n", err.c_str());
                return;
            }

            const char* msgType = doc["type"] | "";
            if (strcmp(msgType, "connected") == 0) return;
            if (strcmp(msgType, "voiced") == 0) {
                if (serverSpeechDetectedCb_) serverSpeechDetectedCb_();
                return;
            }
            if (strcmp(msgType, "accepted") == 0) {
                if (acceptedCb_) acceptedCb_();
                if (processingCb_) processingCb_(true);
                return;
            }
            if (strcmp(msgType, "final") == 0) {
                if (finalCb_) finalCb_();
                const char* voiceText =
                    doc["metadata"]["voice_text"] | doc["voice_text"] |
                    static_cast<const char*>(nullptr);
                const char* responseText =
                    doc["metadata"]["text"] | doc["text"] | static_cast<const char*>(nullptr);
                if (finalTextCb_) finalTextCb_(responseText, voiceText);
                if (processingCb_) processingCb_(false);
                return;
            }
            if (strcmp(msgType, "stop") == 0) {
                if (stopCb_) stopCb_();
                return;
            }
            if (strcmp(msgType, "error") == 0 || strcmp(msgType, "canceled") == 0) {
                if (processingCb_) processingCb_(false);
                return;
            }
            if (strcmp(msgType, "tool_call") == 0) {
                const char* toolName =
                    doc["metadata"]["tool_call"]["name"] | static_cast<const char*>(nullptr);
                if (toolName && toolCallCb_) toolCallCb_(toolName);
                return;
            }
            if (strcmp(msgType, "vision") == 0) {
                Serial.println("[WS] vision requested");
                if (visionCb_) visionCb_();
                return;
            }

            if (strcmp(msgType, "start") != 0 && strcmp(msgType, "chunk") != 0) return;
            if (strcmp(msgType, "start") == 0) {
                const char* requestText =
                    doc["metadata"]["request_text"] | static_cast<const char*>(nullptr);
                if (startCb_) startCb_(requestText);
                if (processingCb_) processingCb_(true);
            }

            if (audioChunkCb_) {
                IncomingAudioChunk chunk = {};
                chunk.type = msgType;
                JsonVariant audioFormat = doc["metadata"]["audio_format"];
                JsonVariant pcmFormat = doc["metadata"]["pcm_format"];
                AudioFormat wireFormat = {};
                wireFormat.codec = audioFormat["codec"] | "pcm16";
                wireFormat.sampleRate = audioFormat["sample_rate"] | pcmFormat["sample_rate"] | 0;
                wireFormat.channels = audioFormat["channels"] | pcmFormat["channels"] | 1;
                wireFormat.bitsPerSample =
                    audioFormat["bits_per_sample"] | pcmFormat["bits_per_sample"] | 16;
                chunk.codec = wireFormat.codec;
                chunk.sampleRate = wireFormat.sampleRate;
                chunk.channels = wireFormat.channels;
                chunk.bitsPerSample = wireFormat.bitsPerSample;
                chunk.faceName =
                    doc["avatar_control_request"]["face_name"] | static_cast<const char*>(nullptr);
                chunk.faceDurationSec = doc["avatar_control_request"]["face_duration"] | 2.0f;
                const char* b64 = doc["audio_data"] | static_cast<const char*>(nullptr);
                if (b64 && b64[0] != '\0') {
                    decodeIncomingAudio(b64, strlen(b64), wireFormat, chunk);
                }
                audioChunkCb_(chunk);
            } else if (faceCb_) {
                const char* faceName =
                    doc["avatar_control_request"]["face_name"] | static_cast<const char*>(nullptr);
                if (faceName) {
                    float durationSec = doc["avatar_control_request"]["face_duration"] | 2.0f;
                    faceCb_(faceName, durationSec);
                }
            }
            pumpAudioUpload();
            break;
        }

        default:
            break;
    }
}

}  // namespace aiavatar
