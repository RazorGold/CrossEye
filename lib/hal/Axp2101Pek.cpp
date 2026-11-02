#include "Axp2101Pek.h"

#if FREEINK_DEVICE_WAVESHARE397

#include <Arduino.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>

namespace Axp2101Pek {
namespace {

constexpr uint8_t ADDR = 0x34;
constexpr uint8_t REG_COMMON_CONFIG = 0x10;  // bit0: soft power off
constexpr uint8_t REG_PEK_LEVEL = 0x27;      // bits1:0 on-time, bits3:2 off-time
constexpr uint8_t REG_PWRON_STATUS = 0x20;   // what powered the PMIC on: b0 PWRON key, b2 VBUS insert
constexpr uint8_t REG_INTEN2 = 0x41;         // bit0/1: PEK positive/negative edge
constexpr uint8_t REG_INTSTS2 = 0x49;        // write 1 to clear
// PWRON is active-low: the negative edge (bit1) is the press, the positive edge
// (bit0) is the release.
// -DAXP2101_PEK_SWAP_EDGES flips this if a unit's log shows presses arriving as bit0.
#ifdef AXP2101_PEK_SWAP_EDGES
constexpr uint8_t PEK_RELEASE = 0x02;
constexpr uint8_t PEK_PRESS = 0x01;
#else
constexpr uint8_t PEK_RELEASE = 0x01;
constexpr uint8_t PEK_PRESS = 0x02;
#endif
constexpr uint8_t PEK_EDGES = PEK_RELEASE | PEK_PRESS;

constexpr uint8_t PEK_ON_1S = 2;   // XPOWERS_POWERON_1S
constexpr uint8_t PEK_OFF_4S = 0;  // XPOWERS_POWEROFF_4S

constexpr uint32_t POLL_MS = 10;
constexpr uint32_t TAP_MIN_HOLD_MS = 40;  // a tap fully inside one poll still registers
constexpr uint32_t STUCK_MS = 10000;      // a lost edge must not read as an endless hold

bool readReg(uint8_t reg, uint8_t& out) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADDR, static_cast<uint8_t>(1)) != 1) return false;
  out = Wire.read();
  return true;
}

bool writeReg(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

bool pressed = false;
uint32_t pressedAt = 0;
uint32_t holdUntil = 0;
uint32_t lastPoll = 0;
uint8_t pwronStatus = 0;

}  // namespace

void begin() {
  uint8_t v;
  if (readReg(REG_PEK_LEVEL, v)) writeReg(REG_PEK_LEVEL, (v & 0xF0) | (PEK_OFF_4S << 2) | PEK_ON_1S);
  if (readReg(REG_INTEN2, v)) writeReg(REG_INTEN2, v | PEK_EDGES);
  writeReg(REG_INTSTS2, PEK_EDGES);
  pressed = false;
  readReg(REG_PWRON_STATUS, pwronStatus);
}

uint8_t powerOnStatus() { return pwronStatus; }

bool poweredOnByVbusOnly() { return (pwronStatus & 0x04) && !(pwronStatus & 0x01); }

uint8_t buttonMask() {
  const uint32_t now = millis();
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    uint8_t sts;
    if (readReg(REG_INTSTS2, sts) && (sts & PEK_EDGES)) {
      writeReg(REG_INTSTS2, sts & PEK_EDGES);
      LOG_DBG("PEK", "edges=0x%02X pressed=%d t=%lu", sts & PEK_EDGES, pressed, static_cast<unsigned long>(now));
      const bool gotPress = sts & PEK_PRESS;
      const bool gotRelease = sts & PEK_RELEASE;
      if (gotPress && gotRelease) {
        // Both in one poll: a tap if idle, otherwise the release of the held key.
        if (pressed) {
          pressed = false;
        } else {
          holdUntil = now + TAP_MIN_HOLD_MS;
        }
      } else if (gotPress) {
        pressed = true;
        pressedAt = now;
      } else if (gotRelease) {
        // A release with no press seen: the key that powered the board on is still
        // being let go during boot. Not a press.
        pressed = false;
      }
    }
    if (pressed && now - pressedAt > STUCK_MS) pressed = false;
  }
  return (pressed || now < holdUntil) ? (1 << InputManager::BTN_POWER) : 0;
}

void powerOff() {
  uint8_t v;
  if (!readReg(REG_COMMON_CONFIG, v)) return;
  writeReg(REG_COMMON_CONFIG, v | 0x01);
  delay(200);  // the rails drop within this window when it works
}

}  // namespace Axp2101Pek

#endif
