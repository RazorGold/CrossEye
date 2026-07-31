#include "WordTokenRule.h"

#include <Utf8.h>

#include <cctype>
#include <cstring>

namespace {

// True for CJK codepoints that are punctuation rather than word content.
// utf8IsCjkBreakable is the *layout* rule and deliberately covers CJK marks,
// because a line may break around them. Counting them as words would inflate a
// Japanese page by every 。 and 「 on it.
bool isCjkPunctuation(const uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0xFE30 && cp <= 0xFE4F)  // CJK Compatibility Forms (vertical punctuation)
         || (cp >= 0xFF01 && cp <= 0xFF0F)  // fullwidth ! through /
         || (cp >= 0xFF1A && cp <= 0xFF20)  // fullwidth : through @
         || (cp >= 0xFF3B && cp <= 0xFF40)  // fullwidth [ through `
         || (cp >= 0xFF5B && cp <= 0xFF60);  // fullwidth { through ｠
}

// True for CJK codepoints that carry word content, i.e. the ones the
// characters-per-word convention divides.
bool isCjkWordCharacter(const uint32_t cp) { return utf8IsCjkBreakable(cp) && !isCjkPunctuation(cp); }

// True for General Punctuation (dashes, bullets, quotes), which appear as
// standalone tokens and are not words.
bool isGeneralPunctuation(const uint32_t cp) { return cp >= 0x2000 && cp <= 0x206F; }

// ParsedText appends a literal '-' to the prefix when it hyphenates a word across
// a line break, so a trailing hyphen marks a fragment whose remainder token
// carries the word. Skipping the prefix also gets words broken at an existing
// hyphen right: those add no character, but the prefix still ends in '-', so the
// two halves count as the one word they were.
bool isHyphenationPrefix(const char* text) {
  const size_t len = strlen(text);
  return len > 1 && text[len - 1] == '-';
}

}  // namespace

bool isWordToken(const char* text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p != 0) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (cp < 0x80) {
      if (std::isalnum(static_cast<unsigned char>(cp))) return true;
      continue;
    }
    // A malformed sequence decodes to the replacement glyph. utf8NextCodepoint
    // advances safely past it, but it is not evidence of word content: counting
    // it would let a damaged block inflate the page's word count.
    if (cp == REPLACEMENT_GLYPH || isGeneralPunctuation(cp) || isCjkPunctuation(cp)) continue;
    return true;
  }
  return false;
}

void PageWordCount::addToken(const char* text) {
  if (text == nullptr || !isWordToken(text) || isHyphenationPrefix(text)) return;

  uint16_t cjkChars = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p != 0) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (isCjkWordCharacter(cp)) cjkChars++;
  }

  if (cjkChars > 0) {
    // Saturate rather than wrap: a page cannot legitimately hold 65535 CJK
    // characters, but a corrupt block should not produce a tiny count.
    cjkCharacters =
        cjkCharacters > UINT16_MAX - cjkChars ? UINT16_MAX : static_cast<uint16_t>(cjkCharacters + cjkChars);
    return;
  }
  if (latinWords < UINT16_MAX) latinWords++;
}

uint16_t PageWordCount::total() const {
  const uint32_t cjkWords = (static_cast<uint32_t>(cjkCharacters) + CJK_CHARS_PER_WORD / 2) / CJK_CHARS_PER_WORD;
  const uint32_t sum = static_cast<uint32_t>(latinWords) + cjkWords;
  return sum > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(sum);
}
