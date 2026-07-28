#pragma once
#include <I18n.h>

#include "fontIds.h"

// Selects the single built-in reader family at compile time.
//
// Each family costs ~1.08 MB of the 6.25 MB OTA slot, so a build ships exactly
// one and everything else is reachable as an SD card font at no flash cost.
// Define READER_FONT_NOTOSERIF for the Noto Serif variant ([env:notoserif]);
// the default is Bitter, the slab serif drawn for e-ink.
//
// READER_FONT_DATA(size, style) resolves to the EpdFontData symbol in
// builtinFonts/all.h, which includes only the selected family's headers.

#ifdef READER_FONT_NOTOSERIF

#define READER_FONT_DATA(size, style) notoserif_##size##_##style
#define READER_FONT_12_ID NOTOSERIF_12_FONT_ID
#define READER_FONT_14_ID NOTOSERIF_14_FONT_ID
#define READER_FONT_16_ID NOTOSERIF_16_FONT_ID
#define READER_FONT_18_ID NOTOSERIF_18_FONT_ID
#define READER_FONT_NAME_STR_ID StrId::STR_NOTO_SERIF

#else

#define READER_FONT_DATA(size, style) bitter_##size##_##style
#define READER_FONT_12_ID BITTER_12_FONT_ID
#define READER_FONT_14_ID BITTER_14_FONT_ID
#define READER_FONT_16_ID BITTER_16_FONT_ID
#define READER_FONT_18_ID BITTER_18_FONT_ID
#define READER_FONT_NAME_STR_ID StrId::STR_BITTER

#endif
