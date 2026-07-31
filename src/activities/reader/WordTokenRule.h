#pragma once
#include <cstdint>

// The one word-token rule, shared by everything that has to agree on what a word
// is: the Words/Min page counter, the dictionary's word selection, and the host
// backfill script. Two devices whose firmware tokenizes differently produce
// incomparable Words/Min figures, so the rule is versioned as TOKEN_RULE_VERSION
// (ReadingStatsUtils.h) and shipped in the sync payload.
//
// Pure: no Epub, no HAL, no Arduino, so test/reading_stats/ covers it directly.

// CJK is counted in characters, not tokens: ParsedText splits a CJK run into one
// token per character, so naive token counting reports a Chinese or Japanese book
// as several times more words than it has. Dividing by a constant is a documented
// convention rather than a measurement — Chinese runs closer to 1.5 characters per
// word and Japanese closer to 2 — but it is stable, versioned, and reachable by the
// script without reimplementing the tokenizer.
constexpr uint16_t CJK_CHARS_PER_WORD = 2;

// True when a token carries word content: an ASCII alphanumeric, or a non-ASCII
// codepoint that is not punctuation. Standalone dashes, bullets and CJK marks are
// tokens but not words.
bool isWordToken(const char* text);

// Accumulates a page's word count one token at a time. Latin-script tokens count
// as one word each; CJK tokens contribute characters, which are converted at the
// end, so the rounding happens once per page rather than once per token.
struct PageWordCount {
  uint16_t latinWords = 0;
  uint16_t cjkCharacters = 0;

  // Applies the token rule and adds the token to whichever tally it belongs in.
  // Tokens that are not words, and hyphenation prefixes (whose remainder token
  // carries the word), are ignored.
  void addToken(const char* text);

  // Latin words plus CJK characters converted to words, rounded to nearest.
  uint16_t total() const;
};
