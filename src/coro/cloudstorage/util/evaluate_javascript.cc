#include "coro/cloudstorage/util/evaluate_javascript.h"

#include <antlr4-runtime.h>

#include <memory>
#include <optional>
#include <sstream>
#include <variant>

#include "coro/cloudstorage/util/antlr/javascript-lexer/javascript_lexer.h"
#include "coro/cloudstorage/util/antlr/javascript-parser/javascript_parser.h"
#include "coro/cloudstorage/util/antlr/javascript-parser/javascript_parserBaseVisitor.h"
#include "coro/cloudstorage/util/antlr/javascript-parser/javascript_parserVisitor.h"
#include "coro/http/http_parse.h"
#include "coro/util/raii_utils.h"

#ifdef _MSC_VER
#pragma warning(disable : 4804 4805)
#endif

namespace coro::cloudstorage::util::js {

namespace {

using ::coro::http::ParseTime;
using ::coro::util::AtScopeExit;
using ::coro_cloudstorage_util_antlr::javascript_lexer;
using ::coro_cloudstorage_util_antlr::javascript_parser;
using ::coro_cloudstorage_util_antlr::javascript_parserBaseVisitor;

class Value;

struct Type {
  std::string name;
};

struct Undefined {};

struct Function {
  std::vector<std::string> args;
  javascript_parser::BlockContext* source;
};

using Array = std::shared_ptr<std::vector<Value>>;
using Variant =
    std::variant<std::nullptr_t, Undefined, int64_t, bool, char, std::string,
                 Array, std::shared_ptr<Value>, Function, Type>;

bool operator==(const Undefined&, const Undefined&) { return true; }

template <typename T>
bool operator==(const T&, const Undefined&) {
  return false;
}

template <typename T>
bool operator==(const Undefined&, const T&) {
  return false;
}

bool operator!=(const Undefined&, const Undefined&) { return false; }

template <typename T>
bool operator!=(const T&, const Undefined&) {
  return true;
}

template <typename T>
bool operator!=(const Undefined&, const T&) {
  return true;
}

template <typename T>
bool operator<=(const T&, const Undefined&) {
  return false;
}

template <typename T>
bool operator<=(const Undefined&, const T&) {
  return false;
}

template <typename T>
bool operator>=(const T&, const Undefined&) {
  return false;
}

template <typename T>
bool operator>=(const Undefined&, const T&) {
  return false;
}

class JsException : public std::exception {
 public:
  using std::exception::exception;
};

class Value : public Variant {
 public:
  using Variant::variant;

  std::string ToString(std::unordered_set<const void*> printed = {}) const {
    return std::visit(
        [&]<typename V>(const V& value) -> std::string {
          if constexpr (std::is_same_v<V, Array>) {
            printed.insert(value.get());
            std::stringstream sstream;
            sstream << '[';
            bool first = false;
            for (const auto& e : *value) {
              if (first == true) {
                sstream << ", ";
              } else {
                first = true;
              }
              if (auto* array = e.GetIf<Array>()) {
                sstream << (printed.contains(array->get())
                                ? "circular"
                                : e.ToString(printed));
              } else {
                sstream << e.ToString(printed);
              }
            }
            sstream << ']';
            return std::move(sstream).str();
          } else if constexpr (std::is_same_v<V, Function>) {
            std::stringstream sstream;
            sstream << "[Function " << value.source->getText() << "]";
            return std::move(sstream).str();
          } else if constexpr (std::is_same_v<V, std::nullptr_t>) {
            return "null";
          } else if constexpr (std::is_same_v<V, Undefined>) {
            return "undefined";
          } else if constexpr (requires(std::stringstream stream, V v) {
                                 { stream << v };
                               }) {
            std::stringstream sstream;
            sstream << value;
            return std::move(sstream).str();
          } else {
            return "[Object]";
          }
        },
        static_cast<const Variant&>(**this));
  }

  const Value& operator*() const {
    if (const auto* p = std::get_if<std::shared_ptr<Value>>(
            static_cast<const Variant*>(this))) {
      return **p;
    } else {
      return *this;
    }
  }

  Value& operator*() {
    return const_cast<Value&>(**const_cast<const Value*>(this));
  }

  template <typename T>
  const T& Get() const {
    return std::get<T>(**this);
  }

  template <typename T>
  T& Get() {
    return std::get<T>(**this);
  }

  template <typename T>
  const T* GetIf() const {
    return std::get_if<T>(&**this);
  }

  template <typename T>
  T* GetIf() {
    return std::get_if<T>(&**this);
  }

  operator bool() const {
    if (GetIf<std::nullptr_t>()) {
      return false;
    } else if (const auto* string = GetIf<std::string>()) {
      return !string->empty();
    } else if (const int64_t* number = GetIf<int64_t>()) {
      return *number != 0;
    } else if (const bool* b = GetIf<bool>()) {
      return *b;
    } else if (const auto* array = GetIf<Array>()) {
      return true;
    } else {
      return true;
    }
  }

 private:
#define STR(s) #s

#define DEFINE_BINARY_OPERATOR(op)                                           \
  friend Value operator op(const Value& v1, const Value& v2) {               \
    return std::visit(                                                       \
        []<typename T1, typename T2>(const T1& e1, const T2& e2) -> Value {  \
          if constexpr (requires(T1 e1, T2 e2) {                             \
                          { e1 op e2 };                                      \
                        }) {                                                 \
            return Value(e1 op e2);                                          \
          } else {                                                           \
            throw JsException("can't " STR(op) " given types");              \
          }                                                                  \
        },                                                                   \
        static_cast<const Variant&>(*v1), static_cast<const Variant&>(*v2)); \
  }

#define DEFINE_BINARY_MUTATING_OPERATOR(op)                            \
  friend Value& operator op(Value& v1, const Value& v2) {              \
    std::visit(                                                        \
        []<typename T1, typename T2>(T1& e1, const T2& e2) {           \
          if constexpr (requires(T1& e1, T2 e2) {                      \
                          { e1 op e2 };                                \
                        }) {                                           \
            e1 op e2;                                                  \
          } else {                                                     \
            throw JsException("can't " STR(op) " given types");        \
          }                                                            \
        },                                                             \
        static_cast<Variant&>(*v1), static_cast<const Variant&>(*v2)); \
    return v1;                                                         \
  }

#define DEFINE_COMPARE_OPERATOR(op)                                          \
  friend bool operator op(const Value& v1, const Value& v2) {                \
    return std::visit(                                                       \
        []<typename T1, typename T2>(const T1& e1, const T2& e2) -> bool {   \
          if constexpr (!std::is_same_v<T1, std::shared_ptr<Value>> &&       \
                        !std::is_same_v<T2, std::shared_ptr<Value>> &&       \
                        requires(T1 e1, T2 e2) {                             \
                          { e1 op e2 };                                      \
                        }) {                                                 \
            return e1 op e2;                                                 \
          } else {                                                           \
            throw JsException("can't " STR(op) " given types");              \
          }                                                                  \
        },                                                                   \
        static_cast<const Variant&>(*v1), static_cast<const Variant&>(*v2)); \
  }

  DEFINE_BINARY_OPERATOR(+)
  DEFINE_BINARY_OPERATOR(-)
  DEFINE_BINARY_OPERATOR(*)
  DEFINE_BINARY_OPERATOR(/)
  DEFINE_BINARY_OPERATOR(%)
  DEFINE_BINARY_OPERATOR(<<)
  DEFINE_COMPARE_OPERATOR(==)
  DEFINE_COMPARE_OPERATOR(!=)
  DEFINE_COMPARE_OPERATOR(<)
  DEFINE_COMPARE_OPERATOR(<=)
  DEFINE_COMPARE_OPERATOR(>)
  DEFINE_COMPARE_OPERATOR(>=)
  DEFINE_BINARY_MUTATING_OPERATOR(+=)
  DEFINE_BINARY_MUTATING_OPERATOR(-=)
  DEFINE_BINARY_MUTATING_OPERATOR(*=)
  DEFINE_BINARY_MUTATING_OPERATOR(/=)
  DEFINE_BINARY_MUTATING_OPERATOR(%=)

  friend Value operator-(const Value& v) {
    return std::visit(
        []<typename T>(const T& e) -> Value {
          if constexpr (requires(T e) {
                          { -e };
                        }) {
            return -e;
          } else {
            throw JsException("can't negate given type");
          }
        },
        static_cast<const Variant&>(*v));
  }

  friend Value& operator++(Value& v) {
    std::visit(
        []<typename T>(T& e) {
          if constexpr (!std::is_same_v<T, bool> && requires(T e) {
                          { ++e };
                        }) {
            ++e;
          } else {
            throw JsException("can't increment given type");
          }
        },
        static_cast<Variant&>(*v));
    return v;
  }

  friend Value operator++(Value& v, int) {
    Value copy = *v;
    std::visit(
        []<typename T>(T& e) {
          if constexpr (!std::is_same_v<T, bool> && requires(T e) {
                          { e++ };
                        }) {
            e++;
          } else {
            throw JsException("can't increment given type");
          }
        },
        static_cast<Variant&>(*v));
    return copy;
  }

  friend Value& operator--(Value& v) {
    std::visit(
        []<typename T>(T& e) {
          if constexpr (!std::is_same_v<T, bool> && requires(T e) {
                          { --e };
                        }) {
            --e;
          } else {
            throw JsException("can't decrement given type");
          }
        },
        static_cast<Variant&>(*v));
    return v;
  }

  friend Value operator--(Value& v, int) {
    Value copy = *v;
    std::visit(
        []<typename T>(T& e) {
          if constexpr (!std::is_same_v<T, bool> && requires(T e) {
                          { e-- };
                        }) {
            e--;
          } else {
            throw JsException("can't decrement given type");
          }
        },
        static_cast<Variant&>(*v));
    return copy;
  }
};

class JsValueException : public JsException {
 public:
  explicit JsValueException(Value value)
      : JsException(value.ToString().c_str()), value_(std::move(value)) {}

  const Value& value() const { return value_; }

 private:
  Value value_;
};

std::shared_ptr<Value> CreateRef(Value value) {
  if (auto* p = std::get_if<std::shared_ptr<Value>>(&value)) {
    return std::move(*p);
  } else {
    return std::make_shared<Value>(std::move(value));
  }
}

class JavascriptVisitor : public javascript_parserBaseVisitor {
 public:
  explicit JavascriptVisitor(
      std::span<std::pair<std::string_view, Value>> environment) {
    for (auto [name, value] : environment) {
      environment_.Add(name, std::move(value));
    }
  }

 private:
  std::any visitRoot(javascript_parser::RootContext* ctx) override {
    ctx->block()->accept(this);
    if (current_return_) {
      return std::move(current_return_).value();
    } else {
      return Value(Undefined{});
    }
  }

  std::any visitIfStatement(
      javascript_parser::IfStatementContext* ctx) override {
    auto condition = std::any_cast<Value>(ctx->expression()->accept(this));
    if (condition) {
      ctx->statement()->accept(this);
    }
    return nullptr;
  }

  std::any visitReturnStatement(
      javascript_parser::ReturnStatementContext* ctx) override {
    current_return_ = std::any_cast<Value>(ctx->expression()->accept(this));
    return nullptr;
  }

  std::any visitBreakStatement(
      javascript_parser::BreakStatementContext* ctx) override {
    break_pending_ = true;
    return nullptr;
  }

  std::any visitContinueStatement(
      javascript_parser::ContinueStatementContext* ctx) override {
    continue_pending_ = true;
    return nullptr;
  }

  std::any visitExpressionStatement(
      javascript_parser::ExpressionStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    for (auto* expr : ctx->expression()) {
      expr->accept(this);
    }
    return nullptr;
  }

  std::any visitDeclarationStatement(
      javascript_parser::DeclarationStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    return ctx->declaration()->accept(this);
  }

  std::any visitBlockStatement(
      javascript_parser::BlockStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    return ctx->block()->accept(this);
  }

  std::any visitForLoopStatement(
      javascript_parser::ForLoopStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    return ctx->forLoop()->accept(this);
  }

  std::any visitSwitchBlockStatement(
      javascript_parser::SwitchBlockStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    return ctx->switchBlock()->accept(this);
  }

  std::any visitThrowStatement(
      javascript_parser::ThrowStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    throw JsValueException(
        *std::any_cast<Value>(ctx->expression()->accept(this)));
  }

  std::any visitTryCatchBlockStatement(
      javascript_parser::TryCatchBlockStatementContext* ctx) override {
    if (continue_pending_ || break_pending_ || current_return_) {
      return nullptr;
    }
    return ctx->tryCatchBlock()->accept(this);
  }

  std::any visitTryCatchBlock(
      javascript_parser::TryCatchBlockContext* ctx) override {
    try {
      return ctx->block(0)->accept(this);
    } catch (const JsValueException& e) {
      environment_.PushStackFrame();
      auto at_exit = AtScopeExit([&] { environment_.PopStackFrame(); });
      environment_.Add(ctx->Identifier()->getText(), e.value());
      return ctx->block(1)->accept(this);
    } catch (const JsException& e) {
      environment_.PushStackFrame();
      auto at_exit = AtScopeExit([&] { environment_.PopStackFrame(); });
      environment_.Add(ctx->Identifier()->getText(), e.what());
      return ctx->block(1)->accept(this);
    }
  }

  std::any visitOneDeclaration(
      javascript_parser::OneDeclarationContext* ctx) override {
    environment_.Add(ctx->Identifier()->getText(),
                     *std::any_cast<Value>(ctx->expression()->accept(this)));
    return nullptr;
  }

  std::any visitIdentifierExpression(
      javascript_parser::IdentifierExpressionContext* ctx) override {
    return environment_.Get(ctx->Identifier()->getText());
  }

  std::any visitSumExpression(
      javascript_parser::SumExpressionContext* ctx) override {
    return std::any_cast<Value>(ctx->expression(0)->accept(this)) +
           std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitSubExpression(
      javascript_parser::SubExpressionContext* ctx) override {
    return std::any_cast<Value>(ctx->expression(0)->accept(this)) -
           std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitMulExpression(
      javascript_parser::MulExpressionContext* ctx) override {
    return std::any_cast<Value>(ctx->expression(0)->accept(this)) *
           std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitDivExpression(
      javascript_parser::DivExpressionContext* ctx) override {
    return std::any_cast<Value>(ctx->expression(0)->accept(this)) /
           std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitModExpression(
      javascript_parser::ModExpressionContext* ctx) override {
    return std::any_cast<Value>(ctx->expression(0)->accept(this)) %
           std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitBitShiftLeftExpression(
      javascript_parser::BitShiftLeftExpressionContext* ctx) override {
    auto lhs = std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(Undefined{});
  }

  std::any visitParenthesisExpression(
      javascript_parser::ParenthesisExpressionContext* ctx) override {
    std::any result;
    for (auto* expr : ctx->expression()) {
      result = expr->accept(this);
    }
    return result;
  }

  std::any visitUnaryMinusExpression(
      javascript_parser::UnaryMinusExpressionContext* ctx) override {
    return -std::any_cast<Value>(ctx->expression()->accept(this));
  }

  std::any visitPreIncrementExpression(
      javascript_parser::PreIncrementExpressionContext* ctx) override {
    auto expr = std::any_cast<Value>(ctx->expression()->accept(this));
    if (ctx->op->getText() == "++") {
      return ++expr;
    } else {
      return --expr;
    }
  }

  std::any visitPostIncrementExpression(
      javascript_parser::PostIncrementExpressionContext* ctx) override {
    auto expr = std::any_cast<Value>(ctx->expression()->accept(this));
    if (ctx->op->getText() == "++") {
      return expr++;
    } else {
      return expr--;
    }
  }

  std::any visitMutateExpression(
      javascript_parser::MutateExpressionContext* ctx) override {
    auto op = ctx->op->getText();
    auto lhs = std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = std::any_cast<Value>(ctx->expression(1)->accept(this));
    if (op == "+=") {
      return lhs += rhs;
    } else if (op == "-=") {
      return lhs -= rhs;
    } else if (op == "*=") {
      return lhs *= rhs;
    } else if (op == "/=") {
      return lhs /= rhs;
    } else {
      return lhs %= rhs;
    }
  }

  std::any visitConstantExpression(
      javascript_parser::ConstantExpressionContext* ctx) override {
    return ctx->constant()->accept(this);
  }

  std::any visitArrayExpression(
      javascript_parser::ArrayExpressionContext* ctx) override {
    Array array = std::make_shared<std::vector<Value>>();
    for (auto* expr : ctx->expression()) {
      array->push_back(CreateRef(*std::any_cast<Value>(expr->accept(this))));
    }
    return Value(std::move(array));
  }

  std::any visitAssignmentExpression(
      javascript_parser::AssignmentExpressionContext* ctx) override {
    auto target = std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = std::any_cast<Value>(ctx->expression(1)->accept(this));
    if (auto* p = std::get_if<std::shared_ptr<Value>>(&target)) {
      **p = *rhs;
      return Value(std::move(*p));
    } else {
      throw JsException("expression not assignable");
    }
  }

  std::any visitSubscriptExpression(
      javascript_parser::SubscriptExpressionContext* ctx) override {
    auto target = std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto* array = target.GetIf<Array>();
    if (!array) {
      throw JsException("object not an array");
    }
    auto index = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    const auto* i = index.GetIf<int64_t>();
    if (!i) {
      throw JsException("subscript not an integer");
    }
    if (*i < 0) {
      return Value(Undefined{});
    } else if (*i >= (*array)->size()) {
      (*array)->resize(*i + 1, std::make_shared<Value>(Undefined{}));
      return Value((**array)[*i]);
    } else {
      return Value((**array)[*i]);
    }
  }

  std::any visitFunctionExpression(
      javascript_parser::FunctionExpressionContext* ctx) override {
    Function function{.source = ctx->block()};
    for (auto* ident : ctx->Identifier()) {
      function.args.emplace_back(ident->getText());
    }
    return Value(std::move(function));
  }

  std::any visitCallExpression(
      javascript_parser::CallExpressionContext* ctx) override {
    auto expression = ctx->expression();
    auto func = std::any_cast<Value>(expression[0]->accept(this));
    const auto* f = func.GetIf<Function>();
    if (!f) {
      throw JsException("object not a function");
    }
    auto at_exit = AtScopeExit([&] { environment_.PopStackFrame(); });
    environment_.PushStackFrame();

    for (size_t i = 0; i < f->args.size(); i++) {
      environment_.Add(f->args[i], i + 1 < expression.size()
                                       ? *std::any_cast<Value>(
                                             expression[i + 1]->accept(this))
                                       : Undefined{});
    }

    f->source->accept(this);

    if (current_return_) {
      auto result = std::move(*current_return_);
      current_return_ = std::nullopt;
      return result;
    } else {
      return Value(Undefined{});
    }
  }

  std::any visitMethodExpression(
      javascript_parser::MethodExpressionContext* ctx) override {
    auto obj = std::any_cast<Value>(ctx->expression(0)->accept(this));

    return EvaluateBuiltIn(*obj, ctx->Identifier()->getText(), [ctx, this] {
      std::vector<Value> args;
      auto expression = ctx->expression();
      for (size_t i = 1; i < expression.size(); i++) {
        args.emplace_back(std::any_cast<Value>(expression[i]->accept(this)));
      }
      return args;
    });
  }

  std::any visitForLoop(javascript_parser::ForLoopContext* ctx) override {
    if (auto* decl = ctx->init_decl) {
      visitDeclaration(decl);
    } else if (auto* expr = ctx->init_expr) {
      expr->accept(this);
    }
    auto* condition = ctx->cond;
    auto* step = ctx->step;
    while (true) {
      if (condition) {
        auto value = std::any_cast<Value>(condition->accept(this));
        if (!value) {
          break;
        }
      }
      ctx->statement()->accept(this);
      if (continue_pending_) {
        continue_pending_ = false;
      }
      if (break_pending_) {
        break_pending_ = false;
        break;
      }
      if (step) {
        step->accept(this);
      }
    }
    return nullptr;
  }

  std::any visitSwitchBlock(
      javascript_parser::SwitchBlockContext* ctx) override {
    auto value = std::any_cast<Value>(ctx->expression()->accept(this));

    bool execute = false;
    for (auto* case_block : ctx->caseBlock()) {
      if (case_block->constant() && std::any_cast<Value>(visitConstant(
                                        case_block->constant())) == value) {
        execute = true;
      }
      if (execute) {
        for (auto* statement : case_block->statement()) {
          statement->accept(this);
          if (break_pending_) {
            break_pending_ = false;
            return nullptr;
          }
        }
      }
    }
    if (execute) {
      return nullptr;
    }
    for (auto* case_block : ctx->caseBlock()) {
      if (!case_block->constant()) {
        execute = true;
      }
      if (execute) {
        for (auto* statement : case_block->statement()) {
          statement->accept(this);
          if (break_pending_) {
            break_pending_ = false;
            return nullptr;
          }
        }
      }
    }
    return nullptr;
  }

  std::any visitMemberExpression(
      javascript_parser::MemberExpressionContext* ctx) override {
    auto obj = *std::any_cast<Value>(ctx->expression()->accept(this));
    std::string ident = ctx->Identifier()->getText();
    if (const auto* string = obj.GetIf<std::string>()) {
      if (ident == "length") {
        return Value(int64_t(string->size()));
      }
    } else if (const auto* array = obj.GetIf<Array>()) {
      if (ident == "length") {
        return Value(int64_t((*array)->size()));
      }
    }
    throw JsException("invalid member field reference");
  }

  std::any visitOrExpression(
      javascript_parser::OrExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    if (lhs) {
      return Value(true);
    }
    return *std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitAndExpression(
      javascript_parser::AndExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    if (!lhs) {
      return Value(false);
    }
    return *std::any_cast<Value>(ctx->expression(1)->accept(this));
  }

  std::any visitGreaterExpression(
      javascript_parser::GreaterExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs > rhs);
  }

  std::any visitGreaterOrEqualExpression(
      javascript_parser::GreaterOrEqualExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs >= rhs);
  }

  std::any visitLessExpression(
      javascript_parser::LessExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs < rhs);
  }

  std::any visitLessOrEqualExpression(
      javascript_parser::LessOrEqualExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs <= rhs);
  }

  std::any visitEqualExpression(
      javascript_parser::EqualExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs == rhs);
  }

  std::any visitNotEqualExpression(
      javascript_parser::NotEqualExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs != rhs);
  }

  std::any visitStrictEqualExpression(
      javascript_parser::StrictEqualExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs == rhs);
  }

  std::any visitNotStrictEqualExpression(
      javascript_parser::NotStrictEqualExpressionContext* ctx) override {
    auto lhs = *std::any_cast<Value>(ctx->expression(0)->accept(this));
    auto rhs = *std::any_cast<Value>(ctx->expression(1)->accept(this));
    return Value(lhs != rhs);
  }

  std::any visitVoidExpression(
      javascript_parser::VoidExpressionContext* ctx) override {
    ctx->expression()->accept(this);
    return Value(Undefined{});
  }

  std::any visitNewExpression(
      javascript_parser::NewExpressionContext* ctx) override {
    if (ctx->Identifier()->getText() == "Date") {
      auto expression = ctx->expression();
      if (expression.size() != 1) {
        throw JsException("Invalid argument count for constructor.");
      }
      Value value = *std::any_cast<Value>(expression[0]->accept(this));
      return Value(1000 * ParseTime(value.Get<std::string>()));
    } else {
      throw JsException("Unknown type.");
    }
  }

  std::any visitTernaryOperatorExpression(
      javascript_parser::TernaryOperatorExpressionContext* ctx) override {
    auto condition = std::any_cast<Value>(ctx->expression(0)->accept(this));
    if (condition) {
      return ctx->expression(1)->accept(this);
    } else {
      return ctx->expression(2)->accept(this);
    }
  }

  std::any visitConstant(javascript_parser::ConstantContext* ctx) override {
    if (auto* integer = ctx->Integer()) {
      return Value(std::stoll(integer->getText()));
    } else if (auto* string = ctx->String()) {
      auto d = string->getText();
      return Value(d.substr(1, d.size() - 2));
    } else if (auto* regex = ctx->RegularExpressionLiteral()) {
      return Value(regex->getText());
    } else if (auto* real = ctx->Real()) {
      std::stringstream stream;
      stream << real->getText();
      double result;
      stream >> result;
      return Value(int64_t(result));
    } else if (auto* nan = ctx->NotANumber()) {
      return Value(int64_t());
    } else if (auto* object = ctx->object()) {
      return Value(Undefined{});
    } else {
      throw JsException("invalid constant");
    }
  }

  template <typename F>
  Value EvaluateBuiltIn(Value& value, std::string_view method, F f_args) {
    if (const auto* string = value.GetIf<std::string>()) {
      if (method == "split") {
        auto args = f_args();
        if (args.size() != 1) {
          throw JsException("split takes one argument");
        }
        return Split(*string, args[0].Get<std::string>());
      }
    } else if (const auto* type = value.GetIf<Type>()) {
      if (type->name == "String" && method == "fromCharCode") {
        auto args = f_args();
        if (args.size() != 1) {
          throw JsException("fromCharCode takes one argument");
        }
        return Value(static_cast<char>(args[0].Get<int64_t>()));
      } else if (type->name == "console" && method == "log") {
        auto args = f_args();
        for (const Value& v : args) {
          std::cerr << v.ToString() << ' ';
        }
        std::cerr << '\n';
        return Undefined{};
      } else if (type->name == "Math" && method == "pow") {
        auto args = f_args();
        if (args.size() != 2) {
          throw JsException("Math.pow takes two arguments");
        }
        return int64_t(
            std::pow(args[0].Get<int64_t>(), args[1].Get<int64_t>()));
      }
    } else if (auto* array = value.GetIf<Array>()) {
      if (method == "push") {
        auto args = f_args();
        if (args.size() != 1) {
          throw JsException("push takes one argument");
        }
        (*array)->emplace_back(CreateRef(std::move(args[0])));
        return nullptr;
      } else if (method == "join") {
        auto args = f_args();
        if (args.size() != 1) {
          throw JsException("join takes one argument");
        }
        return Join(*array, args[0].Get<std::string>());
      } else if (method == "splice") {
        auto args = f_args();
        if (args.size() < 1) {
          throw JsException("splice takes at least one argument");
        }
        int64_t start = args[0].Get<int64_t>();
        return Splice(*array, start,
                      args.size() >= 2 ? args[1].Get<int64_t>()
                                       : (*array)->size() - start,
                      args.size() >= 3 ? std::span<const Value>(args).subspan(2)
                                       : std::span<const Value>());
      } else if (method == "reverse") {
        std::reverse((*array)->begin(), (*array)->end());
        return value;
      } else if (method == "forEach") {
        auto args = f_args();
        if (args.size() < 1 || args.size() > 2) {
          throw JsException("forEach takes either 1 or 2 arguments");
        }
        ForEach(value, args[0].Get<Function>(),
                args.size() == 2 ? std::make_optional(std::move(args[1]))
                                 : std::nullopt);
        return Undefined{};
      } else if (method == "unshift") {
        auto args = f_args();
        return Unshift(*array, args);
      } else if (method == "pop") {
        return Pop(*array);
      } else if (method == "indexOf") {
        auto args = f_args();
        if (args.size() != 1) {
          throw JsException("indexOf takes one argument");
        }
        return IndexOf(*array, args[0]);
      }
    }
    throw JsException("unimplemented method");
  }

  static Array Split(std::string_view input, std::string_view separator) {
    if (separator != "") {
      throw JsException("nonempty separator unsupported");
    }
    Array result = std::make_shared<std::vector<Value>>();
    for (char c : input) {
      result->emplace_back(CreateRef(char(c)));
    }
    return result;
  }

  static std::string Join(const Array& array, std::string_view separator) {
    if (separator != "") {
      throw JsException("nonempty separator unsupported");
    }
    std::string result;
    for (const auto& v : *array) {
      result += v.ToString();
    }
    return result;
  }

  static int64_t Unshift(Array& array, std::span<Value> args) {
    for (auto it = std::rbegin(args); it != std::rend(args); it++) {
      array->insert(array->begin(), CreateRef(std::move(*it)));
    }
    return array->size();
  }

  static Array Splice(Array& array, int64_t start, int64_t delete_count,
                      std::span<const Value> items) {
    Array result = std::make_shared<std::vector<Value>>();
    int64_t s = start >= 0 ? start : array->size() + start;
    s = std::max<int64_t>(std::min<int64_t>(s, array->size()), 0);
    result->insert(
        result->begin(), array->begin() + s,
        array->begin() + std::min<int64_t>(s + delete_count, array->size()));
    array->erase(
        array->begin() + s,
        array->begin() + std::min<int64_t>(s + delete_count, array->size()));
    for (auto it = std::rbegin(items); it != std::rend(items); it++) {
      array->insert(array->begin() + s, CreateRef(std::move(*it)));
    }
    return result;
  }

  static Value Pop(Array& array) {
    Value result = std::move(array->back());
    array->pop_back();
    return result;
  }

  static int64_t IndexOf(const Array& array, const Value& value) {
    int64_t index = 0;
    for (const auto& e : *array) {
      if (e == value) {
        return index;
      }
      index++;
    }
    return -1;
  }

  void ForEach(const Value& array, const Function& callback,
               std::optional<Value> this_arg) {
    if (callback.args.size() > 3 || callback.args.size() < 1) {
      throw JsException("invalid callback argument for forEach");
    }
    int index = 0;
    for (const auto& e : *array.Get<Array>()) {
      environment_.PushStackFrame();
      auto at_exit = AtScopeExit([&] { environment_.PopStackFrame(); });
      environment_.Add(callback.args[0], **e);
      if (callback.args.size() >= 2) {
        environment_.Add(callback.args[1], index);
      }
      if (callback.args.size() >= 3) {
        environment_.Add(callback.args[2], array);
      }
      if (this_arg) {
        environment_.Add("this", *this_arg);
      }

      callback.source->accept(this);

      index++;
    }
  }

  class Environment {
   public:
    Environment() {
      PushStackFrame();
      Add("String", Type{.name = "String"});
      Add("console", Type{.name = "console"});
      Add("null", nullptr);
      Add("Math", Type{.name = "Math"});
    }

    void PushStackFrame() { stack_.emplace_back(); }
    void PopStackFrame() { stack_.pop_back(); }

    Value Get(std::string_view name) {
      for (auto it = std::rbegin(stack_); it != std::rend(stack_); it++) {
        if (auto value_it = it->find(std::string(name));
            value_it != it->end()) {
          return value_it->second;
        }
      }
      throw JsException("variable undefined");
    }

    void Add(std::string_view name, Value value) {
      const auto [it, success] = stack_.back().try_emplace(
          std::string(name), CreateRef(std::move(value)));
      if (!success) {
        throw JsException("variable redefined");
      }
    }

   private:
    std::vector<std::unordered_map<std::string, Value>> stack_;
  } environment_;
  std::optional<Value> current_return_;
  bool break_pending_ = false;
  bool continue_pending_ = false;
};

std::string EvaluateJavascriptImpl(
    const ::coro::cloudstorage::util::js::Function& function,
    std::span<const std::string> arguments) {
  antlr4::ANTLRInputStream input(function.source);
  javascript_lexer lexer(&input);
  antlr4::CommonTokenStream tokens(&lexer);
  tokens.fill();
  javascript_parser parser(&tokens);
  parser.getInterpreter<antlr4::atn::ParserATNSimulator>()->setPredictionMode(
      antlr4::atn::PredictionMode::SLL);
  antlr4::tree::ParseTree* parse_tree = [&] {
    try {
      return parser.root();
    } catch (const std::exception& e) {
      tokens.reset();
      parser.reset();
      parser.getInterpreter<antlr4::atn::ParserATNSimulator>()
          ->setPredictionMode(antlr4::atn::PredictionMode::LL);
      return parser.root();
    }
  }();
  if (arguments.size() != function.args.size()) {
    throw JsException("invalid argument count");
  }
  std::vector<std::pair<std::string_view, Value>> environment(arguments.size());
  for (size_t i = 0; i < arguments.size(); i++) {
    environment.emplace_back(function.args[i], arguments[i]);
  }
  std::any result = JavascriptVisitor(environment).visit(parse_tree);
  if (Value* output = std::any_cast<Value>(&result)) {
    return output->ToString();
  } else {
    throw JsException("invalid output");
  }
}

}  // namespace

std::string EvaluateJavascript(
    const coro::cloudstorage::util::js::Function& function,
    std::span<const std::string> arguments) {
  return EvaluateJavascriptImpl(function, arguments);
}

}  // namespace coro::cloudstorage::util::js