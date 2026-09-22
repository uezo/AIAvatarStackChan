#pragma once

#include "HardwareAdapter.h"

namespace aiavatar {

class StackChanHardware : public HardwareAdapter {
public:
    StackChanHardware();

    bool begin() override;
    void update() override;
    const char* name() const override { return "StackChan"; }

    bool motionAvailable() const override { return active_; }
    void moveMotion(int16_t yaw, int16_t pitch, uint16_t speed) override;
    bool consumeNadeEvent() override;
    bool ledAvailable() const override { return active_; }
    uint8_t ledCount() const override { return active_ ? 12 : 0; }
    void setLedColor(uint8_t r, uint8_t g, uint8_t b) override;
    void setLedPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void refreshLed() override;

    bool active() const { return active_; }
    void setAutoAngleSyncEnabled(bool enabled);
    bool autoAngleSyncEnabled() const { return autoAngleSyncEnabled_; }
    void setNadeMinTouchIntensity(uint8_t intensity) { nadeMinTouchIntensity_ = intensity; }

private:
    bool active_;
    bool autoAngleSyncEnabled_;
    uint32_t lastStackChanUpdateMs_;
    uint8_t nadeMinTouchIntensity_ = 2;
    uint8_t lastNadeRaw_ = 0xFF;
};

}  // namespace aiavatar
