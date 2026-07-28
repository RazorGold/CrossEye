#pragma once

// The built-in reader family is chosen at build time. Only one is ever
// included, because each costs ~1.08 MB of the 6.25 MB OTA slot -- see
// [env:default] (Bitter) and [env:notoserif] in platformio.ini.
#ifdef READER_FONT_NOTOSERIF
#include <builtinFonts/notoserif_12_bold.h>
#include <builtinFonts/notoserif_12_bolditalic.h>
#include <builtinFonts/notoserif_12_italic.h>
#include <builtinFonts/notoserif_12_regular.h>
#include <builtinFonts/notoserif_14_bold.h>
#include <builtinFonts/notoserif_14_bolditalic.h>
#include <builtinFonts/notoserif_14_italic.h>
#include <builtinFonts/notoserif_14_regular.h>
#include <builtinFonts/notoserif_16_bold.h>
#include <builtinFonts/notoserif_16_bolditalic.h>
#include <builtinFonts/notoserif_16_italic.h>
#include <builtinFonts/notoserif_16_regular.h>
#include <builtinFonts/notoserif_18_bold.h>
#include <builtinFonts/notoserif_18_bolditalic.h>
#include <builtinFonts/notoserif_18_italic.h>
#include <builtinFonts/notoserif_18_regular.h>
#else
#include <builtinFonts/bitter_12_bold.h>
#include <builtinFonts/bitter_12_bolditalic.h>
#include <builtinFonts/bitter_12_italic.h>
#include <builtinFonts/bitter_12_regular.h>
#include <builtinFonts/bitter_14_bold.h>
#include <builtinFonts/bitter_14_bolditalic.h>
#include <builtinFonts/bitter_14_italic.h>
#include <builtinFonts/bitter_14_regular.h>
#include <builtinFonts/bitter_16_bold.h>
#include <builtinFonts/bitter_16_bolditalic.h>
#include <builtinFonts/bitter_16_italic.h>
#include <builtinFonts/bitter_16_regular.h>
#include <builtinFonts/bitter_18_bold.h>
#include <builtinFonts/bitter_18_bolditalic.h>
#include <builtinFonts/bitter_18_italic.h>
#include <builtinFonts/bitter_18_regular.h>
#endif

// UI fonts, family-independent.
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>
