#include "../become.hpp"
#include "./lexer.hpp"

#include <array>
#include <iostream>

#define LLVM_LIKELY(EXPR) __builtin_expect((bool)(EXPR), true)
#define LLVM_UNLIKELY(EXPR) __builtin_expect((bool)(EXPR), false)

// make the lexer create separate tables for
//  each symbol that starts with the same char
//  so we have to match on less chars.
//  This should be done programmaticaly
namespace lexer {

constexpr bool is_hwhitespace(const auto c) noexcept { return c == ' ' || c == '\t'; }

constexpr bool is_vwhitespace(const auto c) noexcept {
    return c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

#define DISPATCH_RETURN int32_t
#define DISPATCH_ARGS_DECL                                                               \
    lexer_t &lexer, const llvm::StringRef src, const pos_t bpos, pos_t pos
#define DISPATCH_ARGS lexer, src, bpos, pos
#define DISPATCH_FNSIG (DISPATCH_ARGS_DECL)->DISPATCH_RETURN

#define asign2table(table, index, val) table[index] = val
constexpr auto one_char_tok_lut = [] consteval {
    std::array<tokc::e, 256> table = {tokc::e::ERROR};
#define TOKEN_ONE_CHAR(spelling, code) asign2table(table, spelling[0], tokc::e::code);
#define TOKEN_SYMETRICAL_OPEN_SEQUENCE(spelling, code, closing_code, sgroup_code)        \
    asign2table(table, spelling[0], tokc::e::code);
#define TOKEN_SYMETRICAL_CLOSE_SEQUENCE(spelling, code, opening_code, sgroup_code)       \
    asign2table(table, spelling[0], tokc::e::code);
#include "../token.def"
    return table;
}();

using dispatch_fn_t = auto DISPATCH_FNSIG;
using dispatch_table_t = std::array<dispatch_fn_t *, 256>;

constexpr auto symbol_sequence_bytes = [] consteval {
    std::array<bool, 256> table = {false};
#define TOKEN_SYMBOL_SEQUENCE(spelling, code)                                            \
    for (std::uint32_t i = 0; i < sizeof(spelling); ++i)                                 \
        asign2table(table, spelling[i], true);
#include "../token.def"

    return table;
}();

constexpr auto id_start_byte_lut = [] consteval {
    std::array<bool, 256> table = {false};
    for (char c = 'A'; c <= 'Z'; ++c)
        table[c] = true;
    for (char c = 'a'; c <= 'z'; ++c)
        table[c] = true;
    table['_'] = true;
    return table;
}();

constexpr auto id_byte_lut = [] consteval {
    std::array<bool, 256> table = id_start_byte_lut;
    for (char c = '0'; c <= '9'; ++c)
        table[c] = true;
    return table;
}();

constexpr auto builtin_byte_lut = [] consteval {
    std::array<bool, 256> table = id_byte_lut;
    asign2table(table, '*', true);
    return table;
}();

constexpr auto symetrical_table = [] consteval {
    std::array<tokc::e, 256> table = {tokc::e::ERROR};
#define TOKEN_SYMETRICAL_OPEN_SEQUENCE(spelling, code, closing_code, sgroup_code)        \
    asign2table(table, (spelling)[0], tokc::e::closing_code);
#define TOKEN_SYMETRICAL_CLOSE_SEQUENCE(spelling, code, opening_code, sgroup_code)       \
    asign2table(table, (spelling)[0], tokc::e::opening_code);
#include "../token.def"
    return table;
}();

constexpr auto parallel_table = [] consteval {
    std::array<tokc::e, 256> table = {tokc::e::ERROR};
#define TOKEN_PARALLEL_SEQUENCE(spelling, code, sgroup_code)                             \
    asign2table(table, spelling[0], tokc::e::code);

#include "../token.def"
    return table;
};
constexpr auto parallel_table_bool = [] consteval {
    std::array<bool, 256> table = {false};
#define TOKEN_PARALLEL_SEQUENCE(spelling, code, sgroup_code)                             \
    asign2table(table, spelling[0], true);

#include "../token.def"
    return table;
};

constexpr auto symetrical_table_bool = [] consteval {
    std::array<bool, 256> table = {false};
#define TOKEN_SYMETRICAL_OPEN_SEQUENCE(spelling, code, closing_code, sgroup_code)        \
    asign2table(table, (spelling)[0], true);
#define TOKEN_SYMETRICAL_CLOSE_SEQUENCE(spelling, code, opening_code, sgroup_code)       \
    asign2table(table, (spelling)[0], true);
#include "../token.def"
    return table;
}();
constexpr auto symetrical_close_table_bool = [] consteval {
    std::array<bool, 256> table = {false};
#define TOKEN_SYMETRICAL_CLOSE_SEQUENCE(spelling, code, opening_code, sgroup_code)       \
    asign2table(table, (spelling)[0], true);
#include "../token.def"
    return table;
}();

constexpr auto whitespace_table = [] consteval {
    std::array<bool, 256> table = {false};
    table[' '] = true;
    table['\n'] = true;
    table['\t'] = true;
    return table;
}();

[[clang::always_inline]] inline auto scan_for_word(llvm::StringRef src, pos_t pos)
    -> llvm::StringRef {
    const std::size_t size = src.size();

    while (pos < static_cast<pos_t>(size) &&
           id_byte_lut[static_cast<unsigned char>(src[pos])])
        ++pos;
    return src.substr(0, pos);
}

template <auto table>
[[clang::always_inline]] inline auto scan_for(llvm::StringRef src, pos_t pos)
    -> llvm::StringRef {
    const std::size_t size = src.size();

    while (pos < static_cast<pos_t>(size) && table[static_cast<unsigned char>(src[pos])])
        ++pos;
    return src.substr(0, pos);
}

[[clang::always_inline]] inline auto scan_for_number(llvm::StringRef src, pos_t pos)
    -> llvm::StringRef {
    const std::size_t size = src.size();
    while (pos < static_cast<pos_t>(size) && '0' <= src[pos] && src[pos] <= '9')
        ++pos;
    return src.substr(0, pos);
}

[[clang::always_inline]] inline auto scan_for_symbol(llvm::StringRef src, pos_t pos)
    -> llvm::StringRef {
    const std::size_t size = src.size();
    while (pos < static_cast<pos_t>(size) && symbol_sequence_bytes[src[pos]])
        ++pos;
    return src.substr(0, pos);
}

[[clang::always_inline]] inline auto skip_hwhitespace(llvm::StringRef src, pos_t &pos)
    -> void {
    while (LLVM_LIKELY(pos < static_cast<pos_t>(src.size())) &&
           LLVM_UNLIKELY(src[pos] == ' ' || src[pos] == '\t'))
        ++pos;
}
namespace lex {
[[clang::always_inline]] inline auto hwhitespace(llvm::StringRef src, pos_t &pos)
    -> void {
    skip_hwhitespace(src, pos);
}
[[clang::always_inline]] inline auto
vwhitespace(lexer_t &lexer, llvm::StringRef src, pos_t &pos) -> void {
    while (pos < static_cast<pos_t>(src.size()) && (src[pos] == '\n')) {
        ++lexer.line_;
        ++pos;
        lexer.line_begin_ = pos;
    }
}

[[clang::always_inline]] auto word(llvm::StringRef src, pos_t &pos) -> llvm::StringRef {
    const auto idtext = scan_for_word(src.substr(pos), 0);
    pos += idtext.size();
    return idtext;
}
[[clang::always_inline]] auto builtin_word(llvm::StringRef src, pos_t &pos)
    -> llvm::StringRef {
    const auto idtext = scan_for<builtin_byte_lut>(src.substr(pos), 0);
    pos += idtext.size();
    return idtext;
}
[[clang::always_inline]] auto number(llvm::StringRef src, pos_t &pos) -> llvm::StringRef {
    // CCHECK(id_start_byte_lut[src[pos]]);

    const auto idtext = scan_for_number(src.substr(pos), 0);

    pos += idtext.size();
    return idtext;
}
[[clang::always_inline]] auto symbol(llvm::StringRef src, pos_t &pos) -> llvm::StringRef {
    // CCHECK(id_start_byte_lut[src[pos]]);
    const auto symbol_text = scan_for_symbol(src.substr(pos), 0);
    pos += symbol_text.size();

    return symbol_text;
}
} // namespace lex

namespace dispatch {
auto next DISPATCH_FNSIG;

[[clang::noinline]]
auto err DISPATCH_FNSIG {
    std::cerr << "Line: " << lexer.line_ << '\n';
    std::cerr << "Error: Unexpected char \' " << src[pos] << " \' at index " << pos
              << '\n';
    std::exit(EXIT_FAILURE);
}

[[clang::noinline, clang::preserve_all]]
auto err(const std::string_view s, DISPATCH_ARGS_DECL) -> DISPATCH_RETURN {
    std::cerr << "Error: " << s << "\n\tPos:" << pos << '\n';

    std::exit(EXIT_FAILURE);
}
auto symbol DISPATCH_FNSIG;

auto hwhitespace DISPATCH_FNSIG {
    lex::hwhitespace(src, pos);
    become next(DISPATCH_ARGS);
}
auto vwhitespace DISPATCH_FNSIG {
    lex::vwhitespace(lexer, src, pos);
    become next(DISPATCH_ARGS);
}

auto comment DISPATCH_FNSIG {
    if (pos + 1 < src.size() && src[pos + 1] == '/') {
        pos += 2;
        while (pos < src.size() && src[pos] != '\n') {
            if (is_hwhitespace(src[pos]))
                lex::hwhitespace(src, pos);
            else if (is_vwhitespace(src[pos]))
                lex::vwhitespace(lexer, src, pos);
            else
                ++pos;
        }
        if (is_hwhitespace(src[pos]))
            lex::hwhitespace(src, pos);
        else if (is_vwhitespace(src[pos]))
            lex::vwhitespace(lexer, src, pos);
        else
            ++pos;
    } else {
        become symbol(DISPATCH_ARGS);
    }
    become next(DISPATCH_ARGS);
}

// acording to perf my biggest bottleneck is this one
auto id DISPATCH_FNSIG {
    const auto text = lex::word(src, pos);

    if (LLVM_UNLIKELY(text.size() == 0))
        return err("ID needs to have charactes in it", DISPATCH_ARGS);

    constexpr tokc::e type = tokc::ID;
    lexer.push(type, bpos, pos);
    become next(DISPATCH_ARGS);
}
auto integral_id DISPATCH_FNSIG {
    const auto text = lex::word(src, pos);

    if (LLVM_UNLIKELY(text.size() == 0))
        return err("ID needs to have charactes in it", DISPATCH_ARGS);

    tokc::e type = tokc::ID;
    // floats have a specific size so they do not really need to be here but it is
    // still ok
    if (text.size() != 1 && std::all_of(text.begin() + 1,
                                        text.end(),
                                        [] [[gnu::const]] (const unsigned char c) {
                                            return std::isdigit(c);
                                        })) {
        switch (text[0]) {
#warning "i and s are used to define signed ints, I should probably just pick 1"
        case 'i':
            type = tokc::TYPE_INT;
            break;
        case 's':
            type = tokc::TYPE_INT;
            break;
        case 'u':
            type = tokc::TYPE_UINT;
            break;
        case 'f':
            type = tokc::TYPE_FLOAT;
            break;
        case 'b':
            type = tokc::TYPE_BOOLEAN;
            break;
        }
    } else if (text == "bool") {
        type = tokc::TYPE_BOOLEAN;
    }

    lexer.push(type, bpos, pos);
    become next(DISPATCH_ARGS);
}

auto number DISPATCH_FNSIG {
    auto text = lex::number(src, pos);
    if (LLVM_UNLIKELY(text.size() == 0))
        return err("Number needs to have numbers in it", DISPATCH_ARGS);

    tokc::e type = tokc::e::INT;
    bool dot = false;
AGAIN:
    if (LLVM_LIKELY(pos < src.size())) {
        if (src[pos] == '.') { /* float */
            ++pos;
            type = tokc::e::FLOAT;
            lex::number(src, pos);
            if (LLVM_UNLIKELY(src[pos] == '.') || dot)
                return err("A float number can only have one dot", DISPATCH_ARGS);
            dot = true;
            goto AGAIN;
        } else if (src[pos] == '\'') { /* int */
            if (!dot)
                type = tokc::e::INT;
            // do {
            ++pos;
            lex::number(src, pos);
            goto AGAIN;
            // } while (LLVM_UNLIKELY(src[pos] == '\''));
        }
    }

    if (LLVM_LIKELY(pos < src.size() && LLVM_UNLIKELY(src[pos] == 'e'))) { /* exponent */
        ++pos;
        if (LLVM_LIKELY(src[pos] == '+' || src[pos] == '-'))
            ++pos;
        else
            err("Unknown exponent sign", DISPATCH_ARGS);

        const auto exp = lex::number(src, pos);
        if (LLVM_UNLIKELY(exp.size() == 0))
            err("Need to have a valid Exponent", DISPATCH_ARGS);
    }

    // TABLE 3
    // and this one
    static constexpr auto invalid_follower_table = [] consteval -> auto {
        std::array<uint8_t, 256> table{}; // Initialize all to 0 (valid)
        table.fill(0);
        table[' '] = true;
        table['\t'] = true;
        table['\n'] = true;
        table['\r'] = true;
        table['\f'] = true;
        table['\v'] = true;
#define TOKEN_SYMBOL_SEQUENCE(spelling, code)                                            \
    for (std::uint32_t i = 0; i < sizeof(spelling); ++i)                                 \
        asign2table(table, spelling[i], true);
#include "../token.def"
        table[';'] = true;
        table[0] = true;
#define TOKEN_SYMETRICAL_CLOSE_SEQUENCE(spelling, code, opening_code, sgroup_code)       \
    asign2table(table, (spelling)[0], true);
#include "../token.def"

        return table;
    }();

    // if (pos < src.size() && !whitespace_table[src[pos]]
    //     && !symbol_sequence_bytes[src[pos]] && src[pos] != ';'
    //     && !symetrical_close_table_bool[src[pos]]) [[unlikely]]

    if (pos < src.size() && !invalid_follower_table[src[pos]]) [[unlikely]]
        err("Invalid following symbol after a number ", DISPATCH_ARGS);

    lexer.push(type, bpos, pos);
    become next(DISPATCH_ARGS);
}

// this is good idea to do
//  automaticaly build tables that lead to a function that leads to tables
//
//  Get all the possible symbols
//  sort them by length
//  match usinging that list

auto symbol DISPATCH_FNSIG {
    auto text = lex::symbol(src, pos);
    if (LLVM_UNLIKELY(text.size() == 0))
        return err("Symbol string needs to have symbols in it", DISPATCH_ARGS);

    auto subbegin = text.begin();
    auto len = text.size();
#define TOKEN_SYMBOL_SEQUENCE(spelling, code) .Case(spelling, tokc::e::code)

    // A smarter person would make sure to make distinctions for the tokens that
    // can only have one char. That person ain't me.
    do {
        const auto view = llvm::StringRef(subbegin, len);
        const auto r = llvm::StringSwitch<tokc::e>(view)
#include "../token.def"
                           .Default(tokc::e::ERROR);

        if (r == tokc::e::ERROR) [[unlikely]] {
            len--;
            pos--;
            if (len == 0) [[unlikely]]
                err("Unknown symbol " + text.str(), DISPATCH_ARGS);
        } else {
            lexer.push(r, bpos, pos);
            break;
        }
    } while (true);

    become next(DISPATCH_ARGS);
}

template <auto &fn> auto onechar DISPATCH_FNSIG {
    const tokc::e t = one_char_tok_lut[src[pos]];
    // CCHECK(LLVM_UNLIKELY(t == tokc::e::ERROR));
    fn(t, DISPATCH_ARGS);

    lexer.push(t, bpos, pos + 1);
    ++pos;

    become next(DISPATCH_ARGS);
}
// static auto onechar DISPATCH_FNSIG {
//   auto t = one_char_tok_lut[src[pos]];
//   // CCHECK(LLVM_UNLIKELY(t == tokc::e::ERROR));
//   // create token
//   // --lexer.depth_;
//   lexer.push(t, bpos, pos + 1);
//   // ++lexer.depth_;
//   ++pos;

//   become next(DISPATCH_ARGS);
// }

// this should also become a symbol with a special function at the front for
// open and close checks
namespace symetrical {

auto open DISPATCH_FNSIG {
    static constexpr auto fn = [](tokc::e r, DISPATCH_ARGS_DECL) constexpr -> void {
        lexer.openstack_.push_back(
            {symetrical_table[src[pos]], lexer.buffer_.toks.size()});
        // ++lexer.depth_;
    };
    become onechar<fn>(DISPATCH_ARGS);
}

auto brace_open DISPATCH_FNSIG {
    if (pos + 1 < src.size() && src[pos + 1] == '[') [[unlikely]] {
        lexer.openstack_.push_back({tokc::RDBRACE, lexer.buffer_.toks.size()});
        // lexer.openstack_.push_back({symetrical_table[src[pos]],
        // lexer.buffer_.toks.size()});
        pos++;
        lexer.push(tokc::LDBRACE, bpos, pos + 1);
        pos++;
        {
            become next(DISPATCH_ARGS);
        }
    }
    become open(DISPATCH_ARGS);
}

static constexpr auto close_check = [](tokc::e r, DISPATCH_ARGS_DECL) constexpr -> auto {
    if (LLVM_UNLIKELY(lexer.openstack_.empty()))
        err("Openstack is empty", DISPATCH_ARGS);
    else if (LLVM_UNLIKELY(lexer.openstack_.back().first != r))
        err("Wrong closing symetrical token", DISPATCH_ARGS);
    // --lexer.depth_;
    auto val = lexer.openstack_.back();
    lexer.openstack_.pop_back();
    return val;
};

auto close DISPATCH_FNSIG {
    {
        // auto t = one_char_tok_lut[src[pos]];
        auto val = close_check(one_char_tok_lut[src[pos]], DISPATCH_ARGS);

        if (LLVM_UNLIKELY(!tokc::is_open_symetrical(lexer.buffer_.toks.back().type_) &&
                          lexer.buffer_.toks.back().type_ != tokc::ENDSTMT))
            lexer.push(tokc::ENDSTMT, bpos, pos + 1);

        lexer.symetrical_index_map[val.second] = lexer.buffer_.toks.size();
        lexer.push(tokc::ENDGROUP, bpos, pos + 1);
        // ++lexer.depth_;
        ++pos;
        {
            become next(DISPATCH_ARGS);
        }
    }

    // become onechar<fn>(DISPATCH_ARGS);
}

auto brace_close DISPATCH_FNSIG {
    if (pos + 1 < src.size() && src[pos + 1] == ']') [[unlikely]] {
        pos++;
        close_check(tokc::RDBRACE, DISPATCH_ARGS);

        if (LLVM_UNLIKELY(!tokc::is_open_symetrical(lexer.buffer_.toks.back().type_) &&
                          lexer.buffer_.toks.back().type_ != tokc::ENDSTMT))
            lexer.push(tokc::ENDSTMT, bpos, pos + 1);

        lexer.push(tokc::ENDGROUP, bpos, pos + 1);
        // ++lexer.depth_;
        ++pos;

        become next(DISPATCH_ARGS);
    }
    become close(DISPATCH_ARGS);
}

// add a recovery token so we can continiue lexing
} // namespace symetrical

auto scolon DISPATCH_FNSIG {
    {
        // auto t = one_char_tok_lut[src[pos]];
        lexer.push(tokc::ENDSTMT, bpos, pos + 1);
        // ++lexer.depth_;
        ++pos;
        {
            become next(DISPATCH_ARGS);
        }
    }

    // become onechar<fn>(DISPATCH_ARGS);
}

auto builtin DISPATCH_FNSIG {
    ++pos;
    auto text = lex::builtin_word(src, pos);
    // for some reason this gave us big perfomance boost
    // instead of using text directly matching the same thing just withough the @
    auto r =
        llvm::StringSwitch<tokc::e>(llvm::StringRef(text.begin() - 1, text.size() + 1))
#define TOKEN_BUILTIN_KEYWORD(spelling, code) .Case(spelling, tokc::e::code)
#include "../token.def"
            .Default(tokc::e::ERROR);

    if (LLVM_UNLIKELY(r == tokc::e::ERROR))
        err("Unknown builtin type: " + text.str(), DISPATCH_ARGS);
    lexer.push(r, bpos, pos);

    become next(DISPATCH_ARGS);
}

auto strlit DISPATCH_FNSIG {
    ++pos;
    while (pos < src.size() && src[pos] != '\"')
        ++pos;
    ++pos;
    lexer.push(tokc::e::STRLIT, bpos, pos);

    become next(DISPATCH_ARGS);
}

constexpr auto dispatch_table = [] consteval {
    dispatch_table_t table = {};
    for (int i = 0; i < 256; ++i) {
        asign2table(table, i, dispatch::err);

        if (id_start_byte_lut[i] == true)
            asign2table(table, i, dispatch::id);
    }

    for (auto i = '0'; i <= '9'; ++i)
        asign2table(table, i, dispatch::number);

    asign2table(table, ' ', dispatch::hwhitespace);
    asign2table(table, '\t', dispatch::hwhitespace);
    asign2table(table, '\n', dispatch::vwhitespace);

    asign2table(table, '@', dispatch::builtin);
    asign2table(table, ';', dispatch::scolon);

#define TOKEN_SYMBOL_SEQUENCE(spelling, code)                                            \
    asign2table(table, (spelling)[0], dispatch::symbol);

// #define TOKEN_ONE_CHAR(spelling, code)                                         \
//   asign2table(table, (spelling)[0], onechar);
#define TOKEN_SYMETRICAL_OPEN_SEQUENCE(spelling, code, closing_code, sgroup_code)        \
    asign2table(table, (spelling)[0], symetrical::open);
#define TOKEN_SYMETRICAL_CLOSE_SEQUENCE(spelling, code, opening_code, sgroup_code)       \
    asign2table(table, (spelling)[0], symetrical::close);
#include "../token.def"
    asign2table(table, '/', comment);

    asign2table(table, '\"', strlit);
    asign2table(table, '[', symetrical::brace_open);
    asign2table(table, ']', symetrical::brace_close);

    asign2table(table, 's', integral_id);
    asign2table(table, 'i', integral_id);
    asign2table(table, 'u', integral_id);
    asign2table(table, 'f', integral_id);
    asign2table(table, 'b', integral_id);

    return table;
}();

[[clang::always_inline]] inline auto next(DISPATCH_ARGS_DECL) -> DISPATCH_RETURN {
    if (LLVM_UNLIKELY(!(pos < static_cast<pos_t>(src.size()))))
        return 0;

    become dispatch_table[static_cast<unsigned char>(src[pos])](lexer, src, pos, pos);
}
} // namespace dispatch

__attribute__((visibility("default"))) auto entry(const src_buffer_t *src)
    -> std::tuple<token_buffer_t, std::map<size_t, size_t>> {
    lexer_t lexer = {.buffer_{
        .src = *src,
    }};

    lexer.buffer_.toks = podlist_t<token_t>::create(64 * 10);
    lexer.buffer_.locs = podlist_t<srcloc_t>::create(64 * 10);
    lexer.openstack_ = podlist_t<std::pair<tokc::e, size_t>>::create(64 * 10);
    lexer.symetrical_index_map = std::map<unsigned long, unsigned long>{};
    // lexer.buffer_.src = src;

    lexer.push(tokc::e::BEGINSYMETRICAL, 0, 0);

    dispatch::next(lexer, src->buffer(), 0, 0);

    if (!tokc::is_open_symetrical(lexer.buffer_.toks.back().type_) &&
        lexer.buffer_.toks.back().type_ != tokc::e::ENDSTMT)
        lexer.push(tokc::e::ENDSTMT, 0, 0);
    lexer.push(tokc::e::ENDGROUP, 0, 0);

    if (lexer.openstack_.size() > 0) {
        std::cerr << "Failed to close a symetrical" << '\n';
        std::exit(EXIT_FAILURE);
    }

    lexer.buffer_.toks.shrink_to_fit();
    lexer.buffer_.locs.shrink_to_fit();

    lexer.openstack_.release();
    return {std::move(lexer.buffer_), lexer.symetrical_index_map};
    // return 0;
}

} // namespace lexer

#undef DISPATCH_RETURN
#undef DISPATCH_ARGS_DECL
#undef DISPATCH_ARGS
