#include <gtest/gtest.h>

#include "WordTokenRule.h"

namespace {

uint16_t countTokens(const std::initializer_list<const char*> tokens) {
  PageWordCount count;
  for (const char* token : tokens) count.addToken(token);
  return count.total();
}

}  // namespace

TEST(WordTokenRule, PlainWordsCount) {
  EXPECT_TRUE(isWordToken("reading"));
  EXPECT_TRUE(isWordToken("Chapter"));
  EXPECT_TRUE(isWordToken("42"));
  EXPECT_TRUE(isWordToken("don't"));
  EXPECT_TRUE(isWordToken("well-known"));
  EXPECT_TRUE(isWordToken("naïve"));
}

TEST(WordTokenRule, PunctuationIsNotAWord) {
  EXPECT_FALSE(isWordToken(""));
  EXPECT_FALSE(isWordToken("."));
  EXPECT_FALSE(isWordToken(",\""));
  EXPECT_FALSE(isWordToken("\xE2\x80\x94"));  // em dash U+2014
  EXPECT_FALSE(isWordToken("\xE2\x80\xA2"));  // bullet U+2022
  EXPECT_FALSE(isWordToken("\xE2\x80\x9C"));  // left double quote U+201C
  EXPECT_FALSE(isWordToken("\xE2\x80\xA6"));  // ellipsis U+2026
  EXPECT_FALSE(isWordToken("\xE3\x80\x82"));  // ideographic full stop U+3002
  EXPECT_FALSE(isWordToken("\xEF\xBC\x81"));  // fullwidth exclamation U+FF01
  EXPECT_FALSE(isWordToken("\xE3\x80\x8C"));  // left corner bracket U+300C
}

TEST(WordTokenRule, MalformedSequencesAreNotWords) {
  // A truncated sequence decodes to the replacement glyph. The scanner must stop
  // at the terminator rather than stepping over it, and a damaged token must not
  // count as word content.
  EXPECT_FALSE(isWordToken("\xE2"));
  EXPECT_FALSE(isWordToken("\xE2\x80"));
  EXPECT_EQ(countTokens({"\xE2\x80", "real"}), 1);
  // Damage next to real content still counts the token once.
  EXPECT_TRUE(isWordToken("re\xE2al"));
}

TEST(WordTokenRule, LatinTokensCountOnePerToken) {
  EXPECT_EQ(countTokens({"the", "quick", "brown", "fox"}), 4);
  EXPECT_EQ(countTokens({"the", ".", "quick", "\xE2\x80\x94", "fox"}), 3);
  EXPECT_EQ(countTokens({}), 0);
}

TEST(WordTokenRule, HyphenationPrefixDoesNotDoubleCount) {
  // ParsedText splits "wonderful" into "wonder-" + "ful" across a line break.
  EXPECT_EQ(countTokens({"wonder-", "ful"}), 1);
  // A word broken at an existing hyphen adds no character, but the prefix still
  // ends in '-', so it lands on the same path and the pair counts once.
  EXPECT_EQ(countTokens({"well-", "known"}), 1);
  // An unsplit hyphenated word is a single token and counts once.
  EXPECT_EQ(countTokens({"well-known"}), 1);
  // A bare hyphen is not a word at all.
  EXPECT_EQ(countTokens({"-"}), 0);
}

TEST(WordTokenRule, CjkCountsCharactersNotTokens) {
  // ParsedText emits one token per CJK character, so four tokens are four
  // characters, which the convention reports as two words.
  const uint16_t words = countTokens({"\xE6\x97\xA5", "\xE6\x9C\xAC", "\xE8\xAA\x9E", "\xE6\x96\x87"});
  EXPECT_EQ(words, 2);

  PageWordCount count;
  count.addToken("\xE6\x97\xA5");
  count.addToken("\xE6\x9C\xAC");
  EXPECT_EQ(count.cjkCharacters, 2);
  EXPECT_EQ(count.latinWords, 0);
}

TEST(WordTokenRule, CjkCharactersRoundToNearestWord) {
  EXPECT_EQ(countTokens({"\xE6\x97\xA5"}), 1);                                  // 1 char  -> 1 word
  EXPECT_EQ(countTokens({"\xE6\x97\xA5", "\xE6\x9C\xAC"}), 1);                  // 2 chars -> 1 word
  EXPECT_EQ(countTokens({"\xE6\x97\xA5", "\xE6\x9C\xAC", "\xE8\xAA\x9E"}), 2);  // 3 chars -> 2 words
}

TEST(WordTokenRule, CjkPunctuationIsNeitherWordNorCharacter) {
  PageWordCount count;
  count.addToken("\xE6\x97\xA5");  // 日
  count.addToken("\xE3\x80\x82");  // 。
  count.addToken("\xE3\x80\x8C");  // 「
  count.addToken("\xE6\x9C\xAC");  // 本
  EXPECT_EQ(count.cjkCharacters, 2);
  EXPECT_EQ(count.latinWords, 0);
  EXPECT_EQ(count.total(), 1);
}

TEST(WordTokenRule, MixedScriptPageAddsBothTallies) {
  PageWordCount count;
  count.addToken("EPUB");
  count.addToken("\xE6\x97\xA5");
  count.addToken("\xE6\x9C\xAC");
  count.addToken("reader");
  EXPECT_EQ(count.latinWords, 2);
  EXPECT_EQ(count.cjkCharacters, 2);
  EXPECT_EQ(count.total(), 3);
}

TEST(WordTokenRule, FullwidthLettersAndDigitsAreWordContent) {
  // Fullwidth A (U+FF21) and 1 (U+FF11) are CJK-breakable but carry content, so
  // they count as characters rather than being dropped as punctuation.
  PageWordCount count;
  count.addToken("\xEF\xBC\xA1");
  count.addToken("\xEF\xBC\x91");
  EXPECT_EQ(count.cjkCharacters, 2);
}

TEST(WordTokenRule, NullTokenIsIgnored) {
  PageWordCount count;
  count.addToken(nullptr);
  EXPECT_EQ(count.total(), 0);
}

TEST(WordTokenRule, TalliesSaturateRatherThanWrap) {
  PageWordCount count;
  count.latinWords = UINT16_MAX;
  count.addToken("word");
  EXPECT_EQ(count.latinWords, UINT16_MAX);

  count.cjkCharacters = UINT16_MAX;
  count.addToken("\xE6\x97\xA5");
  EXPECT_EQ(count.cjkCharacters, UINT16_MAX);
  EXPECT_EQ(count.total(), UINT16_MAX);
}
