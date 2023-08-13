#include "coro/cloudstorage/util/antlr/javascript_lexer_base.h"

#include "coro/cloudstorage/util/antlr/javascript-lexer/javascript_lexer.h"

namespace coro_cloudstorage_util_antlr {

std::unique_ptr<antlr4::Token> JavaScriptLexerBase::nextToken() {
  auto next = antlr4::Lexer::nextToken();

  if (next->getChannel() == antlr4::Token::DEFAULT_CHANNEL) {
    last_token_ = true;
    last_token_type_ = next->getType();
  }

  return next;
}

void JavaScriptLexerBase::reset() {
  last_token_ = false;
  last_token_type_ = 0;
  antlr4::Lexer::reset();
}

bool JavaScriptLexerBase::IsRegexPossible() {
  if (!last_token_) {
    return true;
  }
  switch (last_token_type_) {
    case javascript_lexer::Identifier:
    case javascript_lexer::CloseBracket:
    case javascript_lexer::CloseParenthesis:
    case javascript_lexer::Integer:
    case javascript_lexer::String:
    case javascript_lexer::Increment:
    case javascript_lexer::Decrement:
      return false;
    default:
      return true;
  }
}

}  // namespace coro_cloudstorage_util_antlr