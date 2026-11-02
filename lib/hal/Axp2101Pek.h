#pragma once

#include <BoardConfig.h>

#if FREEINK_DEVICE_WAVESHARE397

#include <stdint.h>

// Waveshare ESP32-S3-ePaper-3.97: the PWR key is wired to the AXP2101 PEK input,
// not to an ESP32 GPIO. This surfaces it to the app as BTN_POWER (press/release
// edges via the PMIC's interrupt status over I2C, SDA41/SCL42, 0x34) and powers
// the board off through the PMIC, which a later PWR press cold-boots.
namespace Axp2101Pek {

// Set the PEK on/off timing (vendor: 1 s on, 4 s off), enable the press/release
// edge interrupts and clear stale ones. I2C (Wire) must already be up — see
// BoardConfig::holdPowerRails().
void begin();

// True when the PMIC reports it was powered on by USB (VBUS) insertion and not by
// the PWR key. Only meaningful right after a cold start (the status is not
// refreshed by an ESP-only reset). Read once in begin().
bool poweredOnByVbusOnly();

// Raw PMIC PWRON_STATUS (reg 0x20) captured in begin(): b0 PWR key, b1 IRQ pin, b2 USB
// insert, b3 battery charge done, b4 battery insert. For boot diagnostics.
uint8_t powerOnStatus();

// InputManager::ButtonHook: returns (1 << BTN_POWER) while PWR is held.
uint8_t buttonMask();

// Cut power through the PMIC. Returns only if the board is still running after
// the request (e.g. an unexpected USB power path), so the caller can fall back.
void powerOff();

}  // namespace Axp2101Pek

#endif
