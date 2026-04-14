#pragma once
#include "./parser.hpp"
#include <optional>

namespace grammar {
struct cursor_helper_t {
    using node_t = grammar::node_t::external_node;
    using span_t = grammar::node_t::median_proxy_t::span_t;
    using cursor_t = span_t::iterator;
    using median_t = grammar::node_t::median_proxy_t;
    using final_t = grammar::node_t::final_t;
    using err_t = grammar::node_t::err_t;

    cursor_helper_t(span_t s) : span_(s), cursor_(s.begin()) {}

    template <auto in = medianc::any,
              typename in_t = decltype(in),
              typename t =
                  std::conditional_t<std::is_same_v<in_t, medianc::e>, median_t, final_t>>
    std::optional<t> extract() {
        auto res = peek<in>();
        if (res)
            cursor_.advance();
        return res;
    }

    template <size_t index, size_t max, auto in, auto... ins>
    auto tuple_extract_impl(auto &tup) -> void {
        if constexpr (index < max) {
            std::get<index>(tup) = extract<in>();
            if constexpr (sizeof...(ins) > 0) {
                return tuple_extract_impl<index + 1, max, ins...>(tup);
            }
        }
    }

    template <auto... ins,
              typename ret = std::tuple<std::optional<
                  std::conditional_t<std::is_same_v<decltype(ins), medianc::e>,
                                     median_t,
                                     final_t>
              >...>>
    auto tuple_extract() -> ret {
        ret tup;
        tuple_extract_impl<0, sizeof...(ins), ins...>(tup);
        return tup;
    }
    template <auto in = medianc::any,
              typename in_t = decltype(in),
              typename t =
                  std::conditional_t<std::is_same_v<in_t, medianc::e>, median_t, final_t>>
    auto must_extract() -> t {
        auto res = extract<in>();
        if (!res) {
            const std::string tstr = [] -> std::string {
                if constexpr (cmp_v<in_t, tokc::e>)
                    return std::string(tokc::str(in));
                else if constexpr (cmp_v<in_t, medianc::e>)
                    return std::string(medianc::str(in));
                else
                    return "";
            }();
            throw std::runtime_error("Expected element not found, " + tstr);
        }
        return *res;
    }

    bool within() const { return span_.contains(cursor_); }
    template <auto in = medianc::any,
              typename in_t = decltype(in),
              typename t =
                  std::conditional_t<std::is_same_v<in_t, medianc::e>, median_t, final_t>>
    std::optional<t> peek() const {
        static_assert(std::is_same_v<in_t, medianc::e> || std::is_same_v<in_t, tokc::e>,
                      "in must be a medianc::e or a tokc::e");

        if (!within() || !std::holds_alternative<t>(cursor_->node())) [[unlikely]]
            return std::nullopt;

        if constexpr (std::is_same_v<t, median_t>) {
            const auto med = cursor_->as_median();
            if constexpr (in == medianc::any) {
                return med; // no advance
            } else {
                if (med.type() == in)
                    return med; // no advance
            }
        } else if constexpr (std::is_same_v<t, final_t>) {
            const auto fin = cursor_->as_final();
            if constexpr (in == tokc::any) {
                return fin; // no advance
            } else {
                if (fin->isa(in))
                    return fin; // no advance
            }
        } else {
            static_assert(false, "input is neither a medianc::e or a tokc::e");
        }
        return std::nullopt;
    }

    // private:
    span_t span_;
    cursor_t cursor_;
};
} // namespace grammar
