#include "profanity_filter.h"
#include "profanity_words_data.h"

#include <pgmspace.h>
#include <ctype.h>

namespace {
constexpr size_t kWordBufMax = 64;

bool isWordChar(char c) {
  return isalnum((unsigned char)c) != 0;
}
}

namespace FoxProfanityFilter {
bool containsBlockedContent(const String& textIn) {
  String text = textIn;
  text.toLowerCase();
  int textLen = (int)text.length();

  char wordBuf[kWordBufMax];
  for (size_t i = 0; i < PROFANITY_WORD_COUNT; i++) {
    const char* wordPtr = (const char*)pgm_read_ptr(&kProfanityWords[i]);
    strncpy_P(wordBuf, wordPtr, kWordBufMax - 1);
    wordBuf[kWordBufMax - 1] = '\0';
    size_t wordLen = strlen(wordBuf);
    if (wordLen == 0) continue;

    bool isPhrase = strchr(wordBuf, ' ') != nullptr;

    int searchFrom = 0;
    while (true) {
      int pos = text.indexOf(wordBuf, searchFrom);
      if (pos < 0) break;

      if (isPhrase) {
        return true;
      }

      bool leftOk = (pos == 0) || !isWordChar(text[pos - 1]);
      int endPos = pos + (int)wordLen;
      bool rightOk = (endPos >= textLen) || !isWordChar(text[endPos]);
      if (leftOk && rightOk) {
        return true;
      }
      searchFrom = pos + 1;
    }
  }
  return false;
}
}
