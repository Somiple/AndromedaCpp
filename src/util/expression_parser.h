#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace andromeda::util {

struct ExpressionError {
    enum class Kind { SyntaxError, ZeroDivision };

    Kind kind = Kind::SyntaxError;
    std::string message;
};

enum class Op { Add, Sub, Mul, Div, Pow };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    struct Int { std::int64_t value; };
    struct UInt { std::uint64_t value; };
    struct Float { double value; };
    struct Var { std::string name; };
    struct UnaryNeg { ExprPtr operand; };
    struct Binary {
        ExprPtr lhs;
        Op op;
        ExprPtr rhs;
    };

    std::variant<Int, UInt, Float, Var, UnaryNeg, Binary> node;
};

class ExpressionParser {
public:
    explicit ExpressionParser(std::string_view text) : text_(text), pos_(0) {}

private:
    std::string_view text_;
    std::size_t pos_;
};

}
