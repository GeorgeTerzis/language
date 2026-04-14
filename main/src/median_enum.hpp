#pragma once

#include "token_enum.hpp"
#include <cstdint>
#include <string_view>
namespace medianc {
  using e_type = tokc::e_type;
enum e : e_type {
  first = tokc::e::last,
#define MEDIAN_CODE(code) code,
#include "median.def"
  any,
  last
};

consteval int64_t length() { return e::last; }

constexpr inline const std::string_view str(e val) {
  switch (val) {
#define MEDIAN_CODE(code)                                                      \
  case code:                                                                   \
    return #code;
#include "median.def"
  default:
    return "Unknown";
  }
}

constexpr inline e symetrical(tokc::e val) {
  switch(val){
    case tokc::LPAREN: return e::PARENS;
    case tokc::LBRACE: return e::BRACES;
    case tokc::LCBRACE: return e::CBRACES;
    case tokc::LDBRACE: return e::DBRACES;

    case tokc::RPAREN: return e::PARENS;
    case tokc::RBRACE: return e::BRACES;
    case tokc::RCBRACE: return e::CBRACES;
    case tokc::RDBRACE: return e::DBRACES;
    default: return e::last;
  }
}


} // namespace medianc
