#include "HalClock.h"

HalClock halClock;

bool HalClock::getDateTime(uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&) const { return false; }
