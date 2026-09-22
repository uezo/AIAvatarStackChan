#include "StackChanHardware.h"

#include <Arduino.h>

#if __has_include(<M5StackChan.h>)
#include <M5StackChan.h>
#define AIAVATAR_HAS_M5STACKCHAN 1
#else
#define AIAVATAR_HAS_M5STACKCHAN 0
#endif

namespace aiavatar {

namespace {

static constexpr uint8_t kStackChanIoExpanderAddress = 0x6F;
static constexpr uint8_t kStackChanVersionRegister = 0x02;
static constexpr uint32_t kStackChanUpdateIntervalMs = 50;

#if AIAVATAR_HAS_M5STACKCHAN
bool detectStackChanHardware() {
    uint8_t version = M5.In_I2C.readRegister8(kStackChanIoExpanderAddress,
                                              kStackChanVersionRegister, 100000);
    return version != 0 && version != 0xFF;
}
#endif

}  // namespace

StackChanHardware::StackChanHardware()
    : active_(false),
      autoAngleSyncEnabled_(false),
      lastStackChanUpdateMs_(0) {}

bool StackChanHardware::begin() {
#if AIAVATAR_HAS_M5STACKCHAN
    if (active_) return true;
    if (!detectStackChanHardware()) {
        Serial.println("[StackChan] hardware not detected");
        return false;
    }
    M5StackChan.begin();
    // Avoid sudden jumps to the servo minimum when a transient position read fails.
    // This can be enabled again from Config for smoother motion if the hardware
    // reads servo positions reliably.
    M5StackChan.Motion.setAutoAngleSyncEnabled(autoAngleSyncEnabled_);
    lastStackChanUpdateMs_ = millis();
    active_ = true;
    Serial.printf("[StackChan] hardware initialized autoAngleSync=%d\n",
                  autoAngleSyncEnabled_ ? 1 : 0);
    return true;
#else
    active_ = false;
    Serial.println("[StackChan] M5StackChan.h is not available");
    return false;
#endif
}

void StackChanHardware::setAutoAngleSyncEnabled(bool enabled) {
    autoAngleSyncEnabled_ = enabled;
#if AIAVATAR_HAS_M5STACKCHAN
    if (active_) {
        M5StackChan.Motion.setAutoAngleSyncEnabled(enabled);
        Serial.printf("[StackChan] autoAngleSync=%d\n", enabled ? 1 : 0);
    }
#endif
}

void StackChanHardware::update() {
#if AIAVATAR_HAS_M5STACKCHAN
    if (!active_) return;

    uint32_t now = millis();
    if (now - lastStackChanUpdateMs_ >= kStackChanUpdateIntervalMs) {
        lastStackChanUpdateMs_ = now;
        M5StackChan.update();
    }
#endif
}

void StackChanHardware::moveMotion(int16_t yaw, int16_t pitch, uint16_t speed) {
#if AIAVATAR_HAS_M5STACKCHAN
    if (active_) M5StackChan.Motion.move(yaw, pitch, speed);
#else
    (void)yaw;
    (void)pitch;
    (void)speed;
#endif
}

bool StackChanHardware::consumeNadeEvent() {
#if AIAVATAR_HAS_M5STACKCHAN
    if (!active_) return false;
    const auto& raw = M5StackChan.TouchSensor.getIntensities();
    const uint8_t packedRaw = raw[0] | (raw[1] << 2) | (raw[2] << 4);
    const bool rawChanged = packedRaw != lastNadeRaw_;
    lastNadeRaw_ = packedRaw;
    if (raw[0] >= nadeMinTouchIntensity_ || raw[1] >= nadeMinTouchIntensity_ ||
        raw[2] >= nadeMinTouchIntensity_) {
        if (rawChanged) {
            Serial.printf("[StackChan] nade by touch raw=%u,%u,%u\n",
                          static_cast<unsigned>(raw[0]), static_cast<unsigned>(raw[1]),
                          static_cast<unsigned>(raw[2]));
        }
        return true;
    }
#endif
    return false;
}

void StackChanHardware::setLedColor(uint8_t r, uint8_t g, uint8_t b) {
#if AIAVATAR_HAS_M5STACKCHAN
    if (!active_) return;
    for (uint8_t i = 0; i < 12; ++i) {
        M5StackChan.setRgbColor(i, r, g, b);
    }
    M5StackChan.refreshRgb();
#else
    (void)r;
    (void)g;
    (void)b;
#endif
}

void StackChanHardware::setLedPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
#if AIAVATAR_HAS_M5STACKCHAN
    if (!active_ || index >= 12) return;
    M5StackChan.setRgbColor(index, r, g, b);
#else
    (void)index;
    (void)r;
    (void)g;
    (void)b;
#endif
}

void StackChanHardware::refreshLed() {
#if AIAVATAR_HAS_M5STACKCHAN
    if (active_) M5StackChan.refreshRgb();
#endif
}

}  // namespace aiavatar
