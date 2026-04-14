#pragma once

#include "../../libs/map.hpp"
#include "../token.hpp"
#include <cstdlib>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSwitch.h>
#include <map>

namespace lexer {
using pos_t = ssize_t;
struct lexer_t {
    token_buffer_t buffer_;
    podlist_t<std::pair<tokc::e, size_t>> openstack_;
    std::map<size_t, size_t> symetrical_index_map;

    std::int32_t line_ = 0;
    std::int32_t line_begin_ = 0;

    [[clang::always_inline]] inline auto
    push(tokc::e type, const pos_t begin, const pos_t end) -> void {
        buffer_.toks.push_back(token_t{type});
        buffer_.locs.push_back(
            srcloc_t{line_ + 1, static_cast<std::int32_t>(end - begin), begin});
    }
};

auto entry(const src_buffer_t* src)
    -> std::tuple<token_buffer_t, std::map<size_t, size_t>>;
}; // namespace lexer
