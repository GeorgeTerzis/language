#pragma once

#include "../median_enum.hpp"
#include "../overloaded.hpp"
#include "../token.hpp"
#include <map>

#include "../../libs/map.hpp"
#include <array>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace grammar {
using cursor_t = podlist_t<token_t>::c_it;

struct node_t {

    struct final_t : public cursor_t {
        using cursor_t::cursor_t;

        final_t(auto *val) : cursor_t(val) {}
        final_t(auto &val) : cursor_t(val) {}
        final_t(auto &&val) : cursor_t(std::move(val)) {}
        final_t(std::nullptr_t) : cursor_t(static_cast<token_t *>(nullptr)) {}

        template <tokc::e TYPE> auto &expect() {
            auto t = this->base()->type();
            if (t != TYPE) [[unlikely]]
                throw std::runtime_error("Expected " + std::string(tokc::str(TYPE)) +
                                         "but have " +
                                         std::string(tokc::str(this->base()->type())));
            return *this;
        }

        template <tokc::e TYPE> const auto &expect() const {
            auto t = this->base()->type();
            if (t != TYPE) [[unlikely]]
                throw std::runtime_error("Expected " + std::string(tokc::str(TYPE)) +
                                         "but have " +
                                         std::string(tokc::str(this->base()->type())));
            return *this;
        }

        auto len() const { return 0; }
        auto type() const { return this->base()->type(); }
    };

    struct median_t {
        using code_t = medianc::e;

        code_t type_;
        std::int32_t len_;

        // I am not sure if I should support this
        //  good for debuging and error messages I guess
        //  but it kinda ballons my size
        // final_t first;
        // final_t last;

        // somehow need a pointer or a reference to the next node or at least be
        // able to generate it

        auto type() const { return type_; }
        auto len() const { return len_; }
    };

    struct err_t {
        std::int16_t index_;
    };

    // template <bool is_const = false>
    struct median_proxy_t {
        struct span_t {
            node_t *parent_;
            node_t *begin_;
            node_t *end_;

            size_t size_ = -1;

            struct iterator {
                using difference_type = std::ptrdiff_t;
                using value_type = node_t;
                using pointer = node_t *;
                using reference = node_t &;
                using iterator_category = std::forward_iterator_tag;

                pointer ptr;

                iterator(pointer p) : ptr(p) {}

                auto operator*() const -> reference { return *ptr; }
                auto operator->() const -> pointer { return ptr; }

                auto next() const -> iterator {
                    return ptr +
                           std::visit(
                               overloaded{[&](const final_t &val) -> size_t { return 1; },
                                          [&](const median_t &val) -> size_t {
                                              // one because the iterator is one behind
                                              // one because that would take us to the
                                              // last element and we want the next element
                                              return ptr->as_median().len() + 2;
                                          },
                                          [&](const err_t &val) -> size_t { return 1; }},
                               ptr->node_);
                }

                auto advance() -> iterator {
                    *this = next();
                    return *this;
                }

                auto operator++() -> iterator & {
                    advance();
                    return *this;
                }

                auto operator++(int) -> iterator {
                    iterator temp = *this;
                    advance();
                    return temp;
                }

                auto operator==(const iterator &other) const -> bool {
                    return ptr == other.ptr;
                }
                auto operator!=(const iterator &other) const -> bool {
                    return ptr != other.ptr;
                }
                auto operator<(const iterator &other) const -> bool {
                    return ptr < other.ptr;
                }
                auto operator>(const iterator &other) const -> bool {
                    return ptr > other.ptr;
                }
                auto operator<=(const iterator &other) const -> bool {
                    return ptr <= other.ptr;
                }
                auto operator>=(const iterator &other) const -> bool {
                    return ptr >= other.ptr;
                }
                auto operator+(int n) const -> iterator {
                    iterator temp = *this;
                    while (n-- > 0) {
                        temp.advance();
                    }
                    return temp;
                }

                auto operator-(int n) const -> iterator {
                    iterator temp = *this;
                    while (n-- > 0) {
                        --temp.ptr; // Move the raw pointer back
                    }
                    return temp;
                }
                auto operator-=(size_t n) -> iterator & {
                    while (n-- > 0) {
                        --ptr; // Move the raw pointer back
                    }
                    return *this;
                }
                auto operator+=(size_t n) -> iterator & {
                    while (n-- > 0) {
                        advance();
                    }
                    return *this;
                }

                auto operator-=(int n) -> iterator & {
                    while (n-- > 0) {
                        --ptr; // Move the raw pointer back
                    }
                    return *this;
                }
            };
            auto subspan(size_t offset, size_t count) const -> span_t {
                if (offset + count > size())
                    throw std::out_of_range("Subspan out of range");
                return span_t{begin_ + offset, begin_ + offset + count};
            }
            auto at(size_t index) const -> auto {
                if (index >= size())
                    throw std::out_of_range("Index out of range");
                return iterator(begin_ + index);
            }
            auto at2(size_t index) const -> auto {
                size_t len = 0;
                auto cursor = begin();
                while (cursor < end() && len < index) {
                    ++len;
                    cursor.advance();
                }
                return cursor;
                // if (index >= size())
                //   throw std::out_of_range("Index out of range");
                // return *(begin_ + index);
            }
            auto operator[](size_t index) const -> node_t & { return *(begin_ + index); }
            auto empty() const -> bool { return begin_ == end_; }

            // raw size
            inline auto size() const -> size_t { return end_ - begin_; }
            inline auto contains(const auto val) const { return (val < end()); }
            // to level size
            auto size2() -> size_t {
                if (size_ != -1)
                    return size_;

                size_ = 0;
                auto cursor = begin();
                while (cursor < end()) {
                    ++size_;
                    cursor.advance();
                }
                return size_;
            }
            inline auto begin() const -> iterator { return iterator(begin_); }
            inline auto end() const -> iterator { return iterator(end_ + 1); }
            inline auto parent() const -> median_proxy_t { return parent_->as_median(); }

            template <bool must_run = true, typename... FNs>
            auto run(FNs... fns) const -> void {
                auto fn_arr = std::array{fns...};
                auto cursor = begin();
                for (auto i = 0; i < fn_arr.size(); i++) {
                    fn_arr[i](cursor);
                    cursor.advance();
                    if (cursor >= end()) {
                        if constexpr (must_run)
                            if (i != fn_arr.size() - 1)
                                std::abort();
                        break;
                    }
                }
            }
        };

        median_proxy_t() : node(nullptr) {}
        median_proxy_t(node_t *val) : node(val), median_(&val->as_raw_median()) {
            // if (!val->is_median()) {
            //     throw std::runtime_error("Expected node to be a median");
            // }
        }
        median_proxy_t(const node_t *val) :
            node(const_cast<node_t *>(val)),
            median_(const_cast<median_t *>(&val->as_raw_median())) {}

        using node_type = node_t;
        node_type *node;
        median_t *median_;

        auto type() const -> median_t::code_t { return median_->type_; }
        auto len() const -> size_t { return children().size(); }

        auto children() const -> span_t {
            node_type *p = node;
            node_type *b = node + 1;

            auto plus = median_->len_;
            node_type *e = node + plus;
            return span_t{p, b, e};
        }

        auto fchild() const -> node_t & {
            node_type *b = node + 1;
            return *(node + 1);
        }

        template <medianc::e... TYPES> auto expect() const {
            if (!((this->type() == TYPES) || ...)) [[unlikely]] {
                std::string expected_types =
                    ((std::string(medianc::str(TYPES)) + " or ") + ...);
                expected_types.erase(expected_types.size() - 4); // remove last " or "
                throw std::runtime_error("Expected " + expected_types + " but have " +
                                         std::string(medianc::str(this->type())));
            }
            return *this;
        }
    };

    using var_t = std::variant<final_t, median_t, err_t>;
    var_t node_;

    [[nodiscard]] auto as_median_opt() -> std::optional<median_proxy_t> { return {this}; }
    [[nodiscard]] auto as_median_opt() const -> std::optional<const median_proxy_t> {
        return {this};
    }

    [[nodiscard]] auto as_median() const -> median_proxy_t { return {this}; }

    [[nodiscard]] auto as_raw_median() -> median_t & { return std::get<median_t>(node_); }
    [[nodiscard]] auto as_raw_median() const -> const median_t & {
        return std::get<median_t>(node_);
    }

    [[nodiscard]] auto as_final() -> final_t & { return std::get<final_t>(node_); }
    [[nodiscard]] auto as_final() const -> const final_t & {
        return std::get<final_t>(node_);
    }

    [[nodiscard]] auto as_err() -> err_t & { return std::get<err_t>(node_); }
    [[nodiscard]] auto as_err() const -> const err_t & { return std::get<err_t>(node_); }

    using external_node = std::variant<std::monostate, final_t, median_proxy_t, err_t>;
    // struct external_node : public ext_node_var {
    //   using ext_node_var::variant;
    //   external_node() : ext_node_var(std::monostate{}) {}
    // };

    auto node() -> external_node {
        if (this->is_median())
            return this->as_median();
        else if (this->is_final())
            return this->as_final();
        else
            std::unreachable();
        // return std::visit(overloaded{[&](median_t& val) -> external_node {
        //                                  return this->as_median();
        //                              },
        //                              [&](auto& val) -> external_node {
        //                                  return val;
        //                              }},
        //                   this->node_);
    }

    template <typename T> [[nodiscard]] auto as() -> auto {
        if constexpr (std::is_same_v<T, median_t> || std::is_same_v<T, median_proxy_t>) {
            return as_median();
        } else if constexpr (std::is_same_v<T, err_t>) {
            return as_err();
        } else {
            static_assert(std::is_same_v<T, final_t>, "Unsupported type for as()");
            return as_final();
        }
    }

    [[nodiscard]] auto is_median() const noexcept -> bool {
        return std::holds_alternative<median_t>(node_);
    }
    [[nodiscard]] auto is_final() const noexcept -> bool {
        return std::holds_alternative<final_t>(node_);
    }
    [[nodiscard]] auto is_err() const noexcept -> bool {
        return std::holds_alternative<err_t>(node_);
    }

    [[nodiscard]] static auto make(cursor_t cursor) -> node_t {
        return node_t{final_t(cursor)};
    }
    [[nodiscard]] static auto make(median_t::code_t type, std::int16_t len, final_t first)
        -> node_t {
        return node_t{median_t{type, len /* first, first */}};
    }
};

using node_buffer_t = podlist_t<node_t>;
using parser_out = podlist_t<node_t>;

struct context_t {
    const token_buffer_t &toks;
    const std::map<size_t, size_t> &smap;
    node_buffer_t &nodes;
};

using median_t = grammar::node_t::median_proxy_t;
using final_t = grammar::node_t::final_t;
using enode_t = grammar::node_t::external_node;

template <auto expected_value> auto expect_median(median_t &val) {
    return val.expect<expected_value>();
}
template <auto expected_value> auto expect_final(final_t &val) {
    return val.expect<expected_value>();
}

template <auto expected_value, typename expected_type> auto expect_node(enode_t &val) {
    using expected_tag_t = expected_type;

    if (std::holds_alternative<std::monostate>(val)) [[unlikely]] {
        throw std::runtime_error("Expected node, but got nothing");
    }

    if constexpr (std::is_same_v<expected_tag_t, final_t>) {
        if (auto *ptr = std::get_if<final_t>(&val)) {
            return expect_final<expected_value>(*ptr);
        } else {
            throw std::runtime_error("Expected final node of type " +
                                     std::string(tokc::str(expected_value)));
        }
    } else if constexpr (std::is_same_v<expected_tag_t, median_t>) {
        if (auto *ptr = std::get_if<median_t>(&val)) {
            return expect_median<expected_value>(*ptr);
        } else {
            throw std::runtime_error("Expected median node of type " +
                                     std::string(medianc::str(expected_value)));
        }
    } else {
        static_assert(sizeof(expected_tag_t) == 0,
                      "Unsupported expected_value type in expect_node");
    }
}

template <auto expected_value, typename T>
    requires(std::is_same_v<decltype(expected_value), medianc::e> ||
             std::is_same_v<decltype(expected_value), tokc::e>)
auto expect(T value) {
    using expected_value_type = decltype(expected_value);
    using expected_type = std::
        conditional_t<std::is_same_v<expected_value_type, medianc::e>,
                      median_t,
                      std::conditional_t<std::is_same_v<expected_value_type, tokc::e>,
                                         final_t,
                                         std::false_type>>;

    if constexpr (std::is_same_v<expected_type, std::false_type>)
        static_assert(false, "Unexpected type");

    if constexpr (std::is_same_v<T, enode_t>) {
        return expect_node<expected_value, expected_type>(value);
    } else if constexpr (std::is_same_v<T, median_t>) {
        return expect_median<expected_value>(value);
    } else if constexpr (std::is_same_v<T, final_t>) {
        return expect_final<expected_value>(value);
    } else if constexpr (std::is_same_v<T, node_t>) {
        return expect<expected_value, T>(value.node());
    } else {
        static_assert(sizeof(T) == 0, "Unsupported node type for expect()");
    }
}

auto entry(const token_buffer_t &toks,
           const std::map<size_t, size_t> &smap,
           cursor_t cursor,
           const cursor_t end) -> parser_out;
// we need those to decipher the templates and anything with ambigous syntax
auto expr(context_t &ctx, cursor_t &cursor) -> void;
// auto type_can_infer(context_t &ctx, cursor_t &cursor) -> void;
auto type(context_t &ctx, cursor_t &cursor) -> void;
auto traverse(const token_buffer_t &buf, std::span<node_t> buffer) -> void;
} // namespace grammar
