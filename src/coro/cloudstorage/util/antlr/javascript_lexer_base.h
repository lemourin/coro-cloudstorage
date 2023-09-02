#ifndef CORO_CLOUDSTORAGE_UTIL_ANTLR_JAVASCRIPT_LEXER_BASE_H
#define CORO_CLOUDSTORAGE_UTIL_ANTLR_JAVASCRIPT_LEXER_BASE_H

#include <antlr4-runtime.h>

namespace coro::cloudstorage::util::antlr {

class JavaScriptLexerBase : public antlr4::Lexer {
 public:
  explicit JavaScriptLexerBase(antlr4::CharStream *input)
      : antlr4::Lexer(input) {}

  std::unique_ptr<antlr4::Token> nextToken() override;
  void reset() override;

  bool IsRegexPossible();

 private:
  bool last_token_ = false;
  size_t last_token_type_ = 0;
};

}  // namespace coro::cloudstorage::util::antlr

#endif  // CORO_CLOUDSTORAGE_UTIL_ANTLR_JAVASCRIPT_LEXER_BASE_H