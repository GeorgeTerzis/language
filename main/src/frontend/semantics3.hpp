// if expresions are borken
//  consult the graph
//  since we do not have a connectio between the parenthesis
//  we need to find all the parenthesis in the chain check how many there are
//  and parse
//  Expresion symbol resolve / chain construction and validation
//  resolver change to a visitor structure instead of a callback one
//  pack ambiguous that belong to the same base to a variant?
//  templates and patterns
//  type checksing
//  and more

// find ambiguous symbols
//  get what symbol they point to
//  build the graph
//  check the graph for cycles

#pragma once
#include "../../libs/llvm_allocator.hpp"
#include "../../libs/map.hpp"
#include "../../libs/meta.hpp"
#include "../../libs/meta_variants.hpp"
#include "../../libs/new_wrapper_allocator.hpp"
#include "../../libs/object_pool.hpp"
#include "../../libs/ref.hpp"
#include "../../libs/set.hpp"
#include "../../libs/vector.hpp"
#include "../mesure.hpp"
#include "./parser.hpp"
#include "cursor_helper.hpp"
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/detail/mp_append.hpp>
#include <boost/mp11/detail/mp_rename.hpp>
#include <boost/mp11/list.hpp>
#include <boost/mp11/tuple.hpp>
#include <boost/pfr.hpp>
#include <boost/type_index.hpp>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <frozen/map.h>
#include <frozen/unordered_map.h>
#include <functional>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Allocator.h>
#include <optional>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

template <typename T>
T strview_num(std::string_view sv) {
    T num = 0;

    auto [ptr, ec] = [](std::string_view sv, T num) {
        if constexpr (std::is_floating_point_v<T>) {
            return std::from_chars(sv.data(), sv.data() + sv.size(), num);
        } else {
            return std::from_chars(sv.data(), sv.data() + sv.size(), num, 10);
        }
    }(sv, num);

    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        throw std::runtime_error("invalid number literal: " + std::string(sv));
    }

    return num;
}
inline std::string source_location_to_string(
    const std::source_location& loc = std::source_location::current()) {
    char buffer[1024];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s:%u in %s",
                  loc.file_name(),
                  loc.line(),
                  loc.function_name());
    return std::string(buffer);
}
inline void
print_source_location(const std::source_location& loc = std::source_location::current()) {
    std::println(stderr, "{}", source_location_to_string(loc));
}
template <typename T, typename... Args>
[[nodiscard]] ref<T> alloc(auto& allocator, Args&&... args) noexcept {
    auto ptr = allocator.template alloc<T>(std::forward<Args>(args)...);
    if (!ptr)
        return nullptr;
    return ref<T>{ptr};
}

namespace semantics {

    using node_t = grammar::node_t::external_node;
    using span_t = grammar::median_t::span_t;
    using cursor_t = span_t::iterator;
    using median_t = grammar::median_t;
    using final_t = grammar::final_t;
    using gerr_t = grammar::node_t::err_t;

    struct decl_t;
    struct type_t;
    struct expr_t;
    struct symbols_t;
    struct stmt_t;
    struct stmts_t;
    struct err_t;
    struct ast_t;

    using ref_decl = ref<decl_t>;
    using ref_symbols = ref<symbols_t>;
    using ref_stmts = ref<stmts_t>;
    using ref_stmt = ref<stmt_t>;
    using ref_expr = ref<expr_t>;
    using ref_type = ref<type_t>;
    using ref_err = ref<err_t>;
    using ref_ast = ref<ast_t>;

    struct err_t {
        const std::string msg;

        err_t(std::string m, std::source_location loc = std::source_location::current()) :
            msg(std::format("Error: {} | {} | {}:{} :: {}",
                            loc.file_name(),
                            loc.function_name(),
                            loc.line(),
                            loc.column(),
                            m)) {}
    };

    template <typename T>
    struct make;

    template <typename T>
    struct make {
        template <typename... Args>
        static T call(Args&&... args) {
            return T{std::forward<Args>(args)...};
        }
    };

    using ast_type_list =
        type_list<ast_t, decl_t, type_t, expr_t, stmt_t, stmts_t, symbols_t>;

    struct pool_t {
        llvm_allocator allocator;
        // could use this for resolving but
        //  since we could multiple allocators this
        //  is not the best of ideas

        vector<ref_decl> decls;
        vector<ref_type> types;
        vector<ref_stmt> stmts;
        vector<ref_expr> exprs;

        auto bytes_allocated() const {
            return allocator.bytes_allocated();
        }

        template <typename T, typename... Args>
        T* alloc(Args&&... args) {
            auto ptr = allocator.alloc<T>(std::move(args)...);
            if constexpr (cmp<T, decl_t>::value)
                decls.emplace_back(ptr);
            if constexpr (cmp<T, type_t>::value)
                types.emplace_back(ptr);
            if constexpr (cmp<T, stmt_t>::value)
                stmts.emplace_back(ptr);
            if constexpr (cmp<T, expr_t>::value)
                exprs.emplace_back(ptr);
            return ptr;
        }
    };

    using allocator_t = pool_t;

    struct external_ctx : uncopyable {
        const token_buffer_t& toks;
        const std::map<size_t, size_t>& smap;
        external_ctx(const token_buffer_t& t, const std::map<size_t, size_t>& sm) :
            toks(t),
            smap(sm) {}
    };

    struct ctx_t : uncopyable {
        external_ctx ectx;

        pool_t& _pool;
        pool_t& scratch_pool;

        auto& spool() {
            return scratch_pool;
        }
        auto& pool() {
            return _pool;
        }

        const auto& toks() const {
            return ectx.toks;
        }
        const auto& smap() const {
            return ectx.smap;
        }

        ctx_t(const token_buffer_t& t,
              const std::map<size_t, size_t>& smap,
              pool_t& p,
              pool_t& sp) :
            ectx(t, smap),
            _pool(p),
            scratch_pool(sp) {}
    };
    using rctx_t = ctx_t;

    namespace nullability {
        enum class e : int8_t { PERMITTED, FORBIDDEN };

        constexpr std::string str(const e v) {
            switch (v) {
            case e::PERMITTED:
                return "permitted";
            case e::FORBIDDEN:
                return "forbidden";
            }
        }
    } // namespace nullability

    namespace mutability {
        enum internal_e : std::int8_t {
            CONSTANT = 0,
            IMMUTABLE = 1,
            MUTABLE = 2,
            NONE = 3
        };

        struct t {
            mutability::internal_e value;

            constexpr t() noexcept : value(NONE) {}
            constexpr t(const mutability::internal_e v) noexcept : value(v) {}
        };

        [[nodiscard]] constexpr bool has(const t v) {
            return v.value != NONE;
        }
        [[nodiscard]] constexpr bool is_none(const t v) {
            return v.value == NONE;
        }
        [[nodiscard]] constexpr bool is_mut(const t v) {
            return v.value == MUTABLE;
        }
        [[nodiscard]] constexpr bool is_imut(const t v) {
            return v.value == IMMUTABLE;
        }
        [[nodiscard]] constexpr bool is_const(const t v) {
            return v.value == CONSTANT;
        }

        [[nodiscard]] constexpr t ifnone(const t v, const t then) {
            if (is_none(v))
                return then;
            return v;
        }

        [[nodiscard]] constexpr bool equals(const t lhs, const t rhs) {
            if (lhs.value == rhs.value)
                return true;

            const t L = ifnone(lhs, rhs);
            const t R = ifnone(rhs, lhs);

            return L.value == R.value;
        }

        [[nodiscard]] constexpr static std::string_view str(const t val) {
            switch (val.value) {
            case NONE:
                return "none";
            case CONSTANT:
                return "constant";
            case IMMUTABLE:
                return "immutable";
            case MUTABLE:
                return "mutable";
            }
        }

        [[nodiscard]] constexpr static mutability::t constant() {
            return CONSTANT;
        }
        [[nodiscard]] constexpr static mutability::t mut() {
            return MUTABLE;
        }
        [[nodiscard]] constexpr static mutability::t imut() {
            return IMMUTABLE;
        }
        [[nodiscard]] constexpr static mutability::t none() {
            return NONE;
        }
    }; // namespace mutability

} // namespace semantics

namespace semantics {

    using base_types = type_list<decl_t, type_t, expr_t, stmt_t, stmts_t>;

    struct symbols_t {
        using entry_t = ref_decl;
        using map_entry_t = std::tuple<std::string_view, entry_t>;

        struct insertion {
            const bool is_inserted;
            const std::string_view name;
            entry_t symbol;
        };

        struct lookup_result {
            ref_symbols where;
            std::optional<entry_t> symbol;
        };

        ref_symbols parent;
        map<std::string_view, entry_t> table = {};

        template <bool is_local>
        static lookup_result lookup_impl(ref_symbols& self, const std::string_view& key) {
            assert(self.is_valid());

            auto it = self.deref().table.find(key);
            if (it != self.deref().table.end())
                return {self, it->second};
            if constexpr (!is_local)
                if (self.deref().parent)
                    return lookup_impl<false>(self.deref().parent, key);
            return {self, std::nullopt};
        }

        static lookup_result local_lookup(ref_symbols& self,
                                          const std::string_view name) {
            return lookup_impl<true>(self, name);
        }

        static lookup_result ancestor_lookup(ref_symbols& self,
                                             const std::string_view name) {
            return lookup_impl<false>(self, name);
        }

        static ref_symbols get_root(ref_symbols current) {
            if (!current.deref().parent)
                return current;
            [[clang::musttail]] return get_root(current.deref().parent);
        }
    };

    struct envpayload_t {
        ref_symbols symbols;
        ref_ast parent;
        strong_ref<pool_t> pool;
    };

    struct dependency {
        ref_ast data;
        vector<ref_ast> deps;
    };

    struct ast_t {
        using var = mp::mp_rename<mp::mp_append<type_list<std::nullptr_t>,
                                                apply2list<ref, base_types>>,
                                  variants>;

        var::t data;
        ref_ast parent;
        vector<ref_ast> children;
    };

    inline auto transfer_ast(ref_ast dst_ast, ref_ast val_ast) {
        assert(val_ast && dst_ast);
        if (val_ast.deref().parent == dst_ast)
            return;

        // we assume that dst_ast doesn't have a holding to val_ast
        auto src = val_ast.deref().parent;
        auto& dst_children = dst_ast.deref().children;

        if (src) {
            auto& src_children = src.deref().children;
            auto it = std::find(src_children.begin(), src_children.end(), val_ast);
            if (it != src_children.end())
                src_children.erase(it);
        }

        dst_children.push_back(val_ast);
        val_ast.deref().parent = dst_ast;
    }

    namespace util {
        struct frame {
            ref_symbols symbols;
            ref_stmts stmts;
        };

        struct field {
            std::string_view name;
            ref_type type;
        };

    } // namespace util

    struct fnsig_t {
        ref_symbols symbols;
        ref_type ret;
        vector<ref_decl> args;
    };

    struct template_inputs_t {
        vector<ref_decl> args;
    };

    template <typename T>
    struct template_input_t {
        using type = T;
        T data;
    };

    struct template_init {
        using list = type_list<ref_decl, ref_type, ref_expr>;

        ref_decl original;

        using input_t = ref_type;
        using inputs_t = vector<input_t>;
        inputs_t inputs;
    };

    namespace decl_structs {
        struct bind_t {
            ref_type type;
            ref_expr init;
        };
        struct variant_member_t {
            ref_type type;
        };
        struct rec_member_t {
            ref_type type;
        };
        struct argument_t {
            ref_type type;
        };

        struct module_t {
            util::frame frame;
        };
        struct module_template_t {
            util::frame frame;
            template_inputs_t template_inputs;
        };
        struct function_t {
            fnsig_t sig;
            ref_expr body;
        };
        struct type_alias_t {
            ref_type ref;
        };
        struct decl_alias_t {
            ref_decl ref;
        };
        struct imut_val_t {
            ref_expr data;
        };

        namespace incomplete {
            struct bind {
                ref_symbols symbols;
                ref_type type;
                std::optional<ref_expr> init_expr;
            };
            struct alias {
                ref_symbols symbols;
                median_t med;
            };
        } // namespace incomplete

        template <typename T>
        struct template_input_t {
            using type = T;
            static constexpr std::string metadata = "template_input";
            T data;
        };

        using ti_decl = template_input_t<decl_alias_t>;
        using ti_type = template_input_t<type_alias_t>;
        using ti_value = template_input_t<imut_val_t>;

        using field_cat = type_list<variant_member_t, rec_member_t, argument_t>;

        using bind_cat = type_list<bind_t, variant_member_t, rec_member_t, argument_t>;

        using incomplete_cat = type_list<incomplete::bind, incomplete::alias>;

        using template_input_cat = type_list<template_input_t<decl_alias_t>,
                                             template_input_t<type_alias_t>,
                                             template_input_t<imut_val_t>>;

        using module_cat = type_list<module_t, module_template_t>;

        using alias_cat = type_list<type_alias_t, decl_alias_t>;

        using callable_cat = type_list<function_t>;

        using variant = mp::mp_rename<mp::mp_append<incomplete_cat,
                                                    template_input_cat,
                                                    module_cat,
                                                    alias_cat,
                                                    callable_cat,
                                                    bind_cat>,
                                      variants>::t;
    } // namespace decl_structs

    namespace type_structs {

        namespace ptr_mut {
            enum e : int8_t { IMMUTABLE, MUTABLE };

            constexpr e imut() {
                return e::IMMUTABLE;
            }
            constexpr e mut() {
                return e::MUTABLE;
            }

            constexpr std::string str(const e v) {
                switch (v) {
                case e::IMMUTABLE:
                    return "immutable";
                case e::MUTABLE:
                    return "mutable";
                }
            }
        } // namespace ptr_mut

        struct const_int {};
        struct const_float {};
        struct const_bool {};

        struct optr_t {};
        struct array_t {
            ref_expr size;
            ref_type type;
        };
        struct ptr_t {
            nullability::e nullability;
            ptr_mut::e mut;
            ref_type type;
        };
        using indirection_cat = type_list<optr_t, array_t, ptr_t>;

        using indirection_variants = mp::mp_rename<indirection_cat, variants>::t;

        struct indirection {
            indirection_variants data;
        };

        struct bitsize_t {
            std::uint16_t size;

            auto operator<=>(const bitsize_t& other) const = default;

            operator unsigned int() {
                return size;
            }
        };
        ;

        /* effectively this is what should be */
        template <size_t Size>

        struct float_base_t {
            static constexpr bitsize_t size = {Size};
        };

        using float16_t = float_base_t<16>;
        using float32_t = float_base_t<32>;
        using float64_t = float_base_t<64>;
        using float128_t = float_base_t<128>;

        using float_cat = type_list<float16_t, float32_t, float64_t, float128_t>;

        // struct float_t {
        //     bitsize_t size;
        //     // check the docs for allowed sizes
        // };

        struct sint_t {
            bitsize_t size;
        };

        struct uint_t {
            bitsize_t size;
        };

        struct bool_t {
            bitsize_t size;
        };

        using const_numeric_cat = type_list<const_float, const_int, const_bool>;
        using numeric_cat = type_list<sint_t, uint_t, bool_t>;
        // using numeric_variants = mp::mp_rename<numeric_list, variants>::t;

        template <typename T>
            requires is_in_list<T, numeric_cat>::value
        bool operator==(const T& lhs, const T& rhs) {
            return lhs.size == rhs.size;
        }

        struct string_t {
            std::string_view name;
        };

        struct variant_t {
            ref_symbols symbols; //@expect variant_member_t
        };

        struct rec_t {
            ref_symbols symbols;
            vector<ref_decl> members;
        };

        struct tup_t {
            vector<ref_type> members;
        };

        using aggregate_category = type_list<rec_t, tup_t>;

        struct fntemplate_t {
            fnsig_t sig;
        };

        struct fntype_t {
            vector<ref_type> args;
            ref_type ret;
        };

        // this one might just get converted to a std::monostate
        struct typeof_t {
            ref_expr expr;
        };

        struct void_t {};

        struct infered_t {
            // this structs whole purpuse is to converted
            //  honestly i shouldn't allow it to be used on functions
        };

        struct alias_t {
            ref_type ref;
        };

        using abstract_category = type_list<void_t, infered_t>;
        using meta_list = type_list<void_t, infered_t, alias_t>;

        struct template_input_t {
            ref_type init;
        };

        // struct incomplete {};
        struct placeholder {};

        namespace incomplete {
            struct alias {
                ref_symbols symbols;
                median_t med;
            };
        }; // namespace incomplete

        using variant = variants<typeof_t,
                                 placeholder,
                                 incomplete::alias,
                                 fntemplate_t,
                                 fntype_t,

                                 template_input_t,

                                 const_int,
                                 const_float,
                                 const_bool,

                                 sint_t,
                                 uint_t,
                                 float16_t,
                                 float32_t,
                                 float64_t,
                                 float128_t,
                                 bool_t,

                                 indirection,

                                 variant_t,
                                 rec_t,
                                 tup_t,
                                 infered_t,
                                 void_t,
                                 alias_t>::t;

    } // namespace type_structs

    namespace expr_structs {
#include "operator.hpp"

        // struct as_expr {
        //     ref_type type;
        //     ref_expr expr;
        // };

        struct infered_t {};

        struct float_t {
            llvm::APFloat val;
        };

        struct int_t {
            llvm::APInt val;
        };

        struct boolean_t {
            bool val;
        };

        struct block_t {
            util::frame frame;
            ref_type type;
        };

        struct complit_t {
            ref_type type;
            vector<ref_expr> init_vals;
        };

        struct variant_init_t {
            std::string_view field;
            ref_expr init;
        };

        struct initlist_t {
            vector<ref_expr> data;
        };

        struct null_t {};

        struct pipe_t {
            ref_expr ref;
        };

        template <typename T>
        struct sizeof_base {
            T val;
        };

        using sizeof_type_t = sizeof_base<ref_type>;
        using sizeof_expr_t = sizeof_base<ref_expr>;

        // this could cause issues with the deep copy system
        struct internal_if {
            ref_expr ctrl_expr;
            ref_stmts body;
            bool is_else() {
                return ctrl_expr.is_null();
            }
        };

        struct if_t {
            vector<internal_if> ifs;
        };
        // it acts like a buffer for when consuming the @as operator
        struct fold_t {
            ref_expr ref;
        };

        namespace incomplete {
            struct chain {
                ref_symbols symbols;
                median_t med;
            };
        } // namespace incomplete

        struct arm_t {
            std::string_view expose_name;
            std::string_view member_name;
            ref_expr expr;
        };

        struct match_t {
            ref_expr val;
            vector<arm_t> arms;
            // arm exprs must have matching types at the very least compatable
            // so a void arm and a s32 arm are not compatable neither are a f32 and a s128
            // or a f32 and s32
        };

        using operand_variants = variants<incomplete::chain,
                                          infered_t,

                                          match_t,

                                          block_t,

                                          variant_init_t,

                                          fold_t,

                                          if_t,
                                          pipe_t,

                                          initlist_t,
                                          complit_t,

                                          null_t,

                                          float_t,
                                          sizeof_type_t,
                                          sizeof_expr_t,
                                          int_t,
                                          boolean_t>::t;
        using numeric_category =
            category<operand_variants, float_t, int_t, boolean_t, null_t>;
        using sizeof_category = category<operand_variants, sizeof_type_t, sizeof_expr_t>;
        using control_flow_category = category<operand_variants, if_t>;

        struct operand_t {
            operand_variants data;
        };

        struct operator_t;

        struct operation_t {
            // I should const this but because of the way I do things I can't
            // since we need to first allocate the space then asign the value
            // thus not allowing us to const the "e type"
            using e = op_operation_e;
            e type;
            [[gnu::const]] constexpr auto meta() const {
                return op_table.at((type));
            }
        };

        struct uop_t {
            struct as_payload_t {
                ref_type type;
            };
            using payload_t = variants<as_payload_t>::t;

            operation_t op;
            ref_expr operand;
            payload_t payload;

            const auto as_payload() {
                if (op.type == operation_t::e::AS) {
                    return payload.get<as_payload_t>();
                } else {
                    throw std::runtime_error("Operator is not an as operator so we "
                                             "can't exctract the as payload");
                }
            }
        };
        struct bop_t {
            operation_t op;
            ref_expr lhs;
            ref_expr rhs;
        };

        inline auto make_uop(op_operation_e op, ref_expr o, uop_t::payload_t p) {
            return uop_t{{op}, o, p};
        }
        inline auto make_bop(op_operation_e op, ref_expr lhs, ref_expr rhs) {
            return bop_t{{op}, lhs, rhs};
        }

        using operator_variants = variants<bop_t, uop_t>::t;
        struct operator_t {
            operator_variants data;
            auto meta() const {
                using ret = const op_meta_t;
                return visit(
                    data,
                    [](const expr_structs::bop_t& val) -> ret { return val.op.meta(); },
                    [](const expr_structs::uop_t& val) -> ret { return val.op.meta(); },
                    [](auto& val) -> ret {
                        throw std::runtime_error("meta throw: " + ::type_str(val));
                    });
            }
        };
        using variant = variants<operand_t, operator_t>::t;
    } // namespace expr_structs

    namespace stmt_structs {

        // struct unwrap_group_t {
        //     vector<ref_decl> binds; // expect unwrap_bind_t
        //     ref_expr init;
        // };

        struct import_t {
            std::string_view file;
        };

        struct become_t {
            ref_expr val;
        };

        struct return_t {
            ref_expr val;
        };

        struct break_t {
            ref_expr val;
        };

        using exit_ctrl_list = type_list<break_t, return_t, become_t>;

        struct decl {
            ref_decl ref;
        };

        struct expr {
            ref_expr ref;
        };

        struct loop_t {
            ref_symbols symbols;
            ref_expr ctrl_expr;
            ref_expr iter_expr;
            ref_expr body_expr;
        };
        struct unreachable {};

        using variant = variants<unreachable,
                                 return_t,
                                 become_t,
                                 break_t,
                                 import_t,
                                 loop_t,
                                 expr,
                                 decl>::t;
    }; // namespace stmt_structs

    struct decl_t {
        ref_ast ast;
        const std::string_view name;
        decl_structs::variant data;
    };

    struct type_t {
        ref_ast ast;

        mutability::t mut;
        type_structs::variant data;
    };

    struct expr_t {
        ref_ast ast;

        ref_type type;
        expr_structs::variant data;
    };

    struct stmt_t {
        ref_ast ast;
        stmt_structs::variant data;
    };

    struct stmts_t {
        ref_ast ast;
        vector<ref_stmt> elms;
    };

    template <typename T>
    struct node_pair {
        ref<T> data;
        ref_ast ast;

        node_pair(ref<T> n, ref_ast a) : data(std::move(n)), ast(std::move(a)) {}
        operator ref<T>&() {
            return data;
        }
        operator ref_ast&() {
            return ast;
        }
    };

    template <>
    struct make<symbols_t> {
        [[nodiscard]] static auto call(auto& allocator, ref_symbols parent) {
            auto ptr = alloc<symbols_t>(allocator, parent);
            return ptr;
        }
    };

    template <typename T>
        requires is_in_list<T, type_structs::numeric_cat>::value
    struct make<T> {
        [[nodiscard]] static auto call(uint16_t size) {
            return T{size};
        }
    };

    template <>
    struct make<type_structs::optr_t> {
        [[nodiscard]] static auto call() {
            return type_structs::indirection{type_structs::optr_t{}};
        }
    };

    template <>
    struct make<type_structs::array_t> {
        [[nodiscard]] static auto call(ref_expr size, ref_type type) {
            return type_structs::indirection{type_structs::array_t{size, type}};
        }
    };

    template <>
    struct make<type_structs::ptr_t> {
        [[nodiscard]] static auto call(type_structs::ptr_mut::e mut, ref_type type) {
            const auto val = type_structs::ptr_t{{}, mut, type};
            return type_structs::indirection{val};
        }
    };

    ref_ast alloc_ast(auto& allocator, ref_ast parent, auto val) {
        ref_ast node = alloc<ast_t>(allocator, val, parent);
        if (auto popt = parent.safe_deref()) {
            auto& pval = popt->get();
            pval.children.emplace_back(node);
        }
        return node;
    }
    ref_expr alloc_expr(auto& allocator,
                        ref_ast parent,
                        ref_type type,
                        expr_structs::variant&& var) {
        ref_expr ptr = alloc<expr_t>(allocator, parent, type, var);
        ref_ast ast = alloc_ast(allocator, parent, ptr);
        ptr.deref().ast = ast;
        return ptr;
    }
    ref_expr alloc_expr(auto& allocator,
                        ref_ast parent,
                        expr_structs::variant&& var,
                        auto type_producer) {
        ref_expr expr_ptr =
            alloc_expr(allocator, parent, ref_type{nullptr}, std::move(var));
        ref_type type_ptr = type_producer(allocator, expr_ptr.deref().ast);
        expr_ptr.deref().type = type_ptr;
        return expr_ptr;
    }

    // probably one of the worst choises I have made
    // a dedicated expr_t, stmt_t, stmts_t, decl_t, type_t would be better
    template <typename T>
        requires is_in_list<T, type_list<decl_t, type_t, stmts_t, stmt_t>>::value
    struct make<T> {
        template <typename... Args>
            requires(sizeof...(Args) == 0 || sizeof...(Args) > 1 ||
                     (sizeof...(Args) == 1 &&
                      !cmp_v<T, typename mp::mp_first<type_list<Args...>>::type>))
        [[nodiscard]] static node_pair<T>
        call(auto& pool, ref_ast parent, Args&&... args) {
            auto node = alloc<T>(pool, nullptr, std::forward<Args>(args)...);
            auto ast = alloc_ast(pool, parent, node);
            node.deref().ast = {ast};
            return {node, ast};
        }
        [[nodiscard]] static node_pair<T> call(auto& pool, ref_ast parent, T val) {
            auto node = alloc<T>(pool, val);
            auto ast = alloc_ast(pool, parent, node);
            node.deref().ast = {ast};
            return {node, ast};
        }
    };

} // namespace semantics

namespace semantics {

    struct env_t {
        using payload_t = envpayload_t;

        rctx_t& ctx;
        payload_t _payload;

        auto& pool() {
            return _payload.pool.deref();
        }
        auto& payload() {
            return _payload;
        }
        auto& symbols() {
            return _payload.symbols;
        }
        auto& parent() {
            return _payload.parent;
        }

      private:
        env_t(rctx_t& ctx, ref_symbols symbols, ref_ast parent, pool_t& pool) :
            ctx(ctx),
            _payload(symbols, parent, &pool) {}

        env_t(rctx_t& ctx, payload_t p) : ctx(ctx), _payload(p) {}

      public:
        template <typename... Args>
        [[nodiscard]]
        env_t with(Args&&... args) const {
            static_assert(sizeof...(Args) > 0, "Call pass instead of with");
            static_assert(sizeof...(Args) <= 3,
                          "Can only pass 3 parameters in this function");

            using list = type_list<Args...>;

            static_assert(mp::mp_size<list>::value ==
                              mp::mp_size<boost::mp11::mp_unique<list>>::value,
                          "Each argument type must be unique (e.g., ref<symtab_t>, "
                          "ref<ast_node_t>)");

            auto new_env = *this;
            mutate_recursion(new_env, args...);
            return new_env;
        }

        [[nodiscard]] env_t pass() {
            return *this;
        }

        friend struct make<env_t>;

      private:
        static void mutate(env_t& env, ref_symbols newsymbols) {
            env._payload.symbols = newsymbols;
        }

        static void mutate(env_t& env, ref_ast parent) {
            env._payload.parent = parent;
        }

        static void mutate(env_t& env, pool_t& pool) {
            env._payload.pool = &pool;
        }

        template <typename Arg, typename... Args>
        static void mutate_recursion(env_t& env, Arg&& arg, Args&&... args) {
            mutate(env, std::forward<Arg>(arg));
            mutate_recursion(env, std::forward<Args>(args)...);
        }

        static void mutate_recursion(env_t& env) {}
    };

    // template <>
    // struct make<env_t> {
    //   template <typename... Args>
    //   static env_t call(Args&&... args) {
    //     return env_t(std::forward<Args>(args)...);
    //   };
    // };

    // easily the most cancerous part of this whole thing
    //  badly made
    //  but
    //  the alternatives for reflection on C++ are pretty much none
    struct deep_copy {
        allocator_t& allocator;
        using ptr_type_list = mp::mp_transform<ref, ast_type_list>;
        using ptr_variants = mp::mp_rename<ptr_type_list, variants>;

        template <typename T>
        struct pair {
            ref<T> old_ptr;
            ref<T> new_ptr;
        };

        using map = map<uintptr_t, ptr_variants::t>;
        map rmap;

        deep_copy(pool_t& ctx) : allocator(ctx) {}

        template <typename T>
        auto place(ref<T> old_ptr, ref<T> new_ptr) {
            rmap.emplace(old_ptr.as_uint(), new_ptr);
        }

        template <typename T>
        ref<T> create(ref<T> old_ptr) {
            if (old_ptr) {
                ref<T> new_ptr = allocator.alloc<T>(old_ptr.deref());
                place(old_ptr, new_ptr);
                return new_ptr;
            }
            return nullptr;
        }

        template <typename T>
        std::optional<pair<T>> retrieve(const uintptr_t key) {
            if (rmap.contains(key)) {
                auto old_ptr = ref<T>{reinterpret_cast<T*>(key)};
                auto new_ptr = rmap.at(key).get<ref<T>>();
                return pair<T>{old_ptr, new_ptr};
            }
            return std::nullopt;
        }

        template <typename T>
        auto expand_map(ref<T> ptr) {
            return expand_map<T>(ptr.deref().ast);
            // auto new_ast = map_builder{*this}.visit(ptr->ast);
            // return new_ast->data.template get<ref<T>>();
        }
        template <typename T>
        auto expand_map(ref_ast ptr) {
            auto new_ast = map_builder{*this}.visit(ptr);
            return new_ast.deref().data.get<ref<T>>();
        }
        auto replace() {
            return replacer::entry(*this);
        }

        static auto entry(deep_copy& self, ref_ast root) {
            ref_ast new_root = nullptr;
            if (root) {
                new_root = map_builder{self}.visit(root);
                self.replace();
            }
            return new_root;
        }

        template <typename T>
        void replace_val_surface(ref<T>& val) {
            auto new_ptr = retrieve<T>(val.as_uint());
            val = new_ptr;
        }

      private:
        struct map_builder {
            deep_copy& self;
            ref_ast visit(ref_ast& old_ptr) {
                auto new_ptr = self.create(old_ptr);
                ::visit(
                    old_ptr.deref().data,
                    [](std::nullptr_t&) {},
                    [](std::monostate&) {},
                    [this](auto& val) { self.create(val); });

                for (auto& elm : old_ptr.deref().children)
                    visit(elm);
                return new_ptr;
            }
        };

        struct replacer {
            deep_copy& self;
            set<void*> visited;

            static void entry(deep_copy& self) {
                replacer{self}.entry_visit();
            }

            auto visit_ref(std::nullptr_t) {}
            auto visit_ref(std::monostate) {}
            template <typename T>
            auto visit_ref(ref<T>& ptr) {
                auto [_, inserted] = visited.insert(ptr.as_void());
                auto belongs = self.rmap.contains(ptr.as_uint());
                if (!inserted || !belongs) {
                    // std::println("Rejection, {}, {}", inserted, belongs);
                    return;
                }

                auto opt = self.retrieve<T>(ptr.as_uint());
                if (opt) {
                    auto [old_ptr, new_ptr] = opt.value();
                    // std::println("{} >> {}", old_ptr.as_void(), new_ptr.as_void());
                    ptr.replace(new_ptr);
                }
            }

            template <typename ComplexT>
            void visit_vector(ComplexT& list) {
                // we need to do the check on the type, we do nto need to do it on every
                constexpr auto fn = metavisit_factory<typename ComplexT::type>();
                if constexpr (cmp_v<std::remove_cvref_t<decltype(fn)>, std::nullopt_t>)
                    return;
                else
                    for (auto& elm : list)
                        fn(this, elm);
            }

            template <typename ComplexT>
            void visit_map(ComplexT& map) {
                constexpr auto fn = metavisit_factory<typename ComplexT::type>();
                if constexpr (cmp_v<std::remove_cvref_t<decltype(fn)>, std::nullopt_t>)
                    return;
                else
                    for (auto& [k, elm] : map)
                        fn(this, elm);
            }

            void visit(ref_ast& ptr) {
                visit_ref(ptr.deref().parent);
                ovisit(ptr.deref().data, [this](auto& val) { visit_ref(val); });
                visit_vector(ptr.deref().children);
            }

            void visit(ref<symbols_t>& ptr) {
                visit_struct(ptr.deref());
            }

            void visit(std::monostate) {}
            void visit(std::nullptr_t) {}

            void entry_visit() {
                for (auto [k, v] : self.rmap) {
                    ::visit(v,
                            overloaded{[this](auto& val) -> void { this->visit(val); }});
                }
            }

            template <class Field>
            static consteval auto metavisit_factory() {
                using type = std::remove_cvref_t<Field>;
                if constexpr (std::is_fundamental_v<type> || std::is_enum_v<type> ||
                              is_in_list<type,
                                         type_list<nullability::e,
                                                   llvm::APFloat,
                                                   llvm::APInt,
                                                   std::optional<ref_expr>,
                                                   type_structs::incomplete::alias,
                                                   median_t,
                                                   expr_structs::incomplete::chain,
                                                   decl_structs::incomplete::bind,
                                                   decl_structs::incomplete::alias,
                                                   stmt_structs::import_t,
                                                   std::string_view,
                                                   type_structs::bitsize_t,
                                                   mutability::t>>::value) {
                    return std::nullopt;
                } else if constexpr (has_metadata_v<type> == "ref") {
                    return [](replacer* t, Field& v) { return t->visit_ref(v); };
                } else if constexpr (has_metadata_v<type> == "variant") {
                    return [](replacer* t, Field& v) { return t->variant_visit(v); };
                } else if constexpr (has_metadata_v<type> == "set") {
                    return [](replacer* t, Field& v) { return t->visit_vector(v); };
                } else if constexpr (has_metadata_v<type> == "map") {
                    return [](replacer* t, Field& v) { return t->visit_map(v); };
                } else if constexpr (has_metadata_v<type> == "vector") {
                    return [](replacer* t, Field& v) { return t->visit_vector(v); };
                } else {
                    return [](replacer* t, Field& v) { return t->visit_struct(v); };
                }
            }

            template <typename T>
            void visit_member(T& val) {
                constexpr auto fn = metavisit_factory<T>();
                if constexpr (cmp_v<std::remove_cvref_t<decltype(fn)>, std::nullopt_t>)
                    return;
                else
                    fn(this, val);
            }

            template <class T>
            void visit_struct(T& s) {
                ::for_each_member(s, [this](auto& v) { this->visit_member(v); });
            }

            void variant_visit(auto& data) {
                // std::cout << type_str(data) << "\n" << std::endl;
                ovisit(data, [this](auto& val) { this->visit_struct(val); });
            }

            void visit(ref<decl_t>& ptr) {
                variant_visit(ptr.deref().data);
            }
            void visit(ref<type_t>& ptr) {
                variant_visit(ptr.deref().data);
            }
            void visit(ref<stmts_t>& ptr) {
                visit_vector(ptr.deref().elms);
            }
            void visit(ref<expr_t>& ptr) {
                variant_visit(ptr.deref().data);
            }
            void visit(ref<stmt_t>& ptr) {
                variant_visit(ptr.deref().data);
            }
        };
    };

    template <typename T>
    ref_symbols get_symbols(ref<T> dptr) {
        auto ptr = dealias(dptr);
        using ret = ref_symbols;
        if constexpr (cmp_v<T, decl_t>) {
            return visit(
                ptr.deref().data,
                [](decl_structs::module_t& val) -> ret { return val.frame.symbols; },
                [](decl_structs::module_template_t& val) -> ret {
                    return val.frame.symbols;
                },
                [](decl_structs::type_alias_t& val) -> ret {
                    return get_symbols(val.ref);
                },
                [](auto&) -> ret { return nullptr; });
        } else if constexpr (cmp_v<T, type_t>) {
            return ::visit(
                ptr.deref().data,
                [](type_structs::rec_t& val) -> ret { return val.symbols; },
                [](type_structs::variant_t& val) -> ret { return val.symbols; },
                [](auto&) -> ret { return nullptr; });
        } else if constexpr (cmp_v<T, expr_t>) {
            static_assert(false, "Can't exctract a symbol table from this Type");
        } else if constexpr (cmp_v<T, stmt_t>) {
            static_assert(false, "Can't exctract a symbol table from this Type");
        } else {
            static_assert(false, "Can't exctract a symbol table from this Type");
        }
    }

    template <typename T>
        requires(cmp_v<T, ref_type> || cmp_v<T, ref_decl>)
    [[nodiscard]] T dealias(const T& ptr) {
        using qtype = std::conditional_t<cmp_v<T, ref_type>,
                                         type_structs::alias_t,
                                         decl_structs::decl_alias_t>;
        // std::println("{} {}", __PRETTY_FUNCTION__, ptr->data.type_str());
        if (auto val = ptr.deref().data.template get_if<qtype>())
            return dealias(val.deref().ref);
        else
            return ptr;
    }

    template <typename T>
        requires(cmp_v<T, type_structs::alias_t> || cmp_v<T, decl_structs::decl_alias_t>)
    [[nodiscard]] cond_t<cmp_v<T, type_structs::alias_t>, ref_type, ref_decl>
    dealias(const T& val) {
        return dealias(val.ref);
    }

    ref_type deref(const ref_type ptr);

    ref_type deref(ref_type ptr, type_structs::indirection& val) {
        return visit(
            val.data,
            [](std::monostate&) -> ref_type { std::unreachable(); },
            [&](type_structs::optr_t&) -> ref_type { return ptr; },
            [](auto& val) -> ref_type { return deref(val.type); });
    }

    ref_type deref(ref_type ptr) {
        return visit(
            ptr.deref().data,
            [&](type_structs::alias_t& val) -> ref_type { return deref(dealias(val)); },
            [&](type_structs::indirection& val) -> ref_type { return deref(ptr, val); },
            [&](auto&) -> ref_type { return ptr; });
    }

    ref_type remove_mutability(pool_t& pool, ref_type ptr) {
        deep_copy dp(pool);
        auto ast = deep_copy::entry(dp, ptr.deref().ast);
        auto type = ast.deref().data.get<ref_type>();
        type.deref().mut = mutability::none();
        return type;
    }

    template <auto cmp_fn, typename VariantWrapper>
    bool equal_template(const VariantWrapper& lhs, const VariantWrapper& rhs) {
        return std::visit(
            [](const auto& lhs, const auto& rhs) -> bool {
                if constexpr (!is_same_type(lhs, rhs))
                    return false;
                else if constexpr (!is_monostate_value(lhs) && !is_monostate_value(rhs))
                    return cmp_fn(lhs, rhs);
                else
                    return true;
            },
            lhs.data,
            rhs.data);
    }

    //@note: aliases are bit weird
    // you might find them in one of the operands of the operation and is should still be
    // correct you just need to dealias them and continiue normaly so I guess template
    // that handles one lhs, one for rhs and one for both?? I do dealiasing at the entry
    // point with ref_type is there a world that you might find it? maybe I should just do
    // it to formalise it?
    //
    // Template inputs are like aliases as well
    //
    // Indirection needs to a bit more thought out I am not 100% sure how to go about it
    // (ALSO NEED TO ALLOW RECURSIVE REFRENCES FOR INDIRECTION
    // the graph system catches them maybe have the indirection remove a connection?
    // have a weak and a strong connection?
    // delay the normal resolution of the symbol under it and pass it to a next stage that
    // just runs it?
    namespace type_structs {

        struct cmp_policy {
            static constexpr bool ignore_mutability = false;
        };

        using trivialy_true = type_list<void_t, optr_t>;
        using trivialy_false =
            type_list<placeholder, incomplete::alias, std::monostate, infered_t>;
        using scafolding = type_list<typeof_t, template_input_t, indirection, alias_t>;

        template <typename Policy = cmp_policy>
        struct equals {
            static bool compare(const ref_type& lhs, const ref_type& rhs) {
                if (lhs == rhs)
                    return true;
                else if (!lhs || !rhs)
                    return false;

                if (!Policy::ignore_mutability) {
                    const auto lhs_mut = lhs.deref().mut;
                    const auto rhs_mut = rhs.deref().mut;

                    if (!mutability::equals(lhs_mut, rhs_mut))
                        return false;
                }

                auto l = dealias(lhs);
                auto r = dealias(rhs);

                return compare(l.deref(), r.deref());
            }

            static bool compare(const type_t& lhs, const type_t& rhs) {
                return compare(lhs.data, rhs.data);
            }

            static constexpr bool compare(const string_t&, const string_t&) {
                return true;
            }

            template <typename T>
                requires is_in_list<T, trivialy_true>::value
            static constexpr bool compare(const T&, const T&) {
                return true;
            }

            template <typename T>
                requires is_in_list<T, trivialy_false>::value
            static constexpr bool compare(const T&, const T&) {
                return false;
            }

            template <typename T>
                requires is_in_list<T, scafolding>::value
            static constexpr bool compare(const T&, const T&) {
                std::println("Scafolding {}", type_str<T>());
                return false;
            }

            template <typename T>
                requires is_in_list<T, numeric_cat>::value
            static bool compare(const T& lhs, const T& rhs) {
                return lhs.size == rhs.size;
            }

            template <typename T>
                requires is_in_list<T, type_structs::float_cat>::value
            static constexpr bool compare(const T&, const T&) {
                return true;
            }

            template <typename T>
                requires is_in_list<T, const_numeric_cat>::value
            static bool compare(const T&, const T&) {
                return true;
            }

            template <typename VariantWrapper>
                requires(has_metadata<VariantWrapper>::value == "variant")
            static bool compare(const VariantWrapper& lhs, const VariantWrapper& rhs) {

                static constexpr auto different_type = [](const auto&, const auto&) {
                    return false;
                };

                static constexpr auto same_type = [](const auto& a, const auto& b) {
                    return compare(a, b);
                };

                static constexpr auto is_monostate = [](const auto&, const auto&) {
                    return false;
                };

                return std::visit(
                    [](const auto& l, const auto& r) -> bool {
                        if constexpr (!is_same_type(l, r))
                            return different_type(l, r);

                        else if constexpr (!is_monostate_value(l) &&
                                           !is_monostate_value(r))
                            return same_type(l, r);

                        else
                            return is_monostate(l, r);
                    },
                    lhs.get_base(),
                    rhs.get_base());
            }

            template <template <typename> class Vector, typename T>
                requires(has_metadata<Vector<T>>::value == "vector")
            static bool compare(const Vector<T>& lhs, const Vector<T>& rhs) {
                return lhs.size() == rhs.size() &&
                       std::equal(
                           lhs.begin(),
                           lhs.end(),
                           rhs.begin(),
                           [](const auto& a, const auto& b) { return compare(a, b); });
            }

            static bool compare(const fntype_t& lhs, const fntype_t& rhs) {
                return compare(lhs.args, rhs.args) && compare(lhs.ret, rhs.ret);
            }

            template <typename T>
            static bool bind_like_structual_equality(const ref_decl& lhs_arg,
                                                     const ref_decl& rhs_arg) {

                auto dlhs = lhs_arg.deref().data.get_if<T>();
                auto drhs = rhs_arg.deref().data.get_if<T>();

                if (!dlhs || !drhs)
                    return false;

                return compare(dlhs.deref().type, drhs.deref().type);
            }

            template <typename T>
            static bool vertical_parameter_equality(const vector<ref_decl>& lhs,
                                                    const vector<ref_decl>& rhs) {

                if (lhs.size() != rhs.size())
                    return false;

                return std::equal(lhs.begin(),
                                  lhs.end(),
                                  rhs.begin(),
                                  bind_like_structual_equality<T>);
            }

            static bool compare(const fnsig_t& lhs, const fnsig_t& rhs) {
                return vertical_parameter_equality<decl_structs::argument_t>(lhs.args,
                                                                             rhs.args) &&
                       compare(lhs.ret, rhs.ret);
            }

            static bool compare(const variant_t& lhs, const variant_t& rhs) {

                return ::equals(lhs.symbols.deref().table,
                                rhs.symbols.deref().table,
                                [](const ref_decl& l, const ref_decl& r) {
                                    return bind_like_structual_equality<
                                        decl_structs::variant_member_t
                                    >(l, r);
                                });
            }

            static bool compare(const rec_t& lhs, const rec_t& rhs) {
                return vertical_parameter_equality<decl_structs::rec_member_t>(
                    lhs.members,
                    rhs.members);
            }

            static bool compare(const tup_t& lhs, const tup_t& rhs) {
                return compare(lhs.members, rhs.members);
            }

            static bool compare(const fntemplate_t& lhs, const fntemplate_t& rhs) {
                return compare(lhs.sig, rhs.sig);
            }
        };

    } // namespace type_structs
    enum class redecl_policy { reuse, unique };
    constexpr redecl_policy redecl_policy_default = redecl_policy::unique;
    template <redecl_policy rp = redecl_policy_default,
              typename NameT,
              typename AllocatorT>
    std::optional<ref_decl>
    alloc_decl(AllocatorT& allocator, ref_ast parent, ref_symbols symbols, NameT name) {

        auto lookup = symbols_t::local_lookup(symbols, name);
        if (lookup.symbol) {
            if constexpr (rp == redecl_policy::unique) {
                return std::nullopt;
            } else {
                return lookup.symbol.value();
            }
        } else {
            auto ptr = make<decl_t>::call(allocator, parent, name, std::monostate{});
            symbols.deref().table.emplace(name, ptr.data);
            return ptr.data;
        }
    };

    template <redecl_policy rp = redecl_policy_default, typename NameT>
    ref_decl alloc_decl(env_t env, NameT name) {
        auto val = alloc_decl<rp, NameT>(env.pool(),
                                         env.parent(),
                                         env.symbols(),
                                         std::forward<const NameT>(name));
        if (!val) {
            throw std::runtime_error("Redeclaration error: symbol '" + std::string(name) +
                                     "' already exists localy.");
        }
        return val.value();
    };

    type_structs::fntype_t fntype_from_fnsig(const fnsig_t& sig) {
        type_structs::fntype_t out;
        for (const auto& arg : sig.args) {
            auto v = arg.deref().data.get_if<decl_structs::bind_t>();
            assert(v);
            out.args.push_back(v.deref().type);
        }

        out.ret = sig.ret;
        return out;
    }

    type_structs::fntype_t fntype_from_fntemplate(const type_structs::fntemplate_t& sig) {
        return fntype_from_fnsig(sig.sig);
    }

    fnsig_t copy_fnsig(env_t env, const fnsig_t& sig) {
        deep_copy dp(env.pool());
        auto new_ret = dp.expand_map(sig.ret);
        vector<ref_decl> new_args;
        new_args.data.reserve(sig.args.size());

        for (auto& elm : sig.args)
            new_args.emplace_back(dp.expand_map(elm));

        auto new_symbols = dp.create(sig.symbols);
        dp.replace();

        return fnsig_t{
            new_symbols,
            new_ret,
            std::move(new_args),
        };
    }

    fnsig_t fnsig_from_fntemplate(env_t env, const type_structs::fntemplate_t& val) {
        return copy_fnsig(env.pass(), val.sig);
    }

    namespace node2ast {
        [[nodiscard]] ref_stmts stmts(env_t env, const span_t span);
        [[nodiscard]] ref_type type(env_t env, const median_t& med);
    } // namespace node2ast

    namespace build {

        auto alloc_type(pool_t& allocator,
                        ref_ast parent,
                        mutability::t mut,
                        type_structs::variant&& d) {
            return make<type_t>::call(allocator, parent, mut, d);
        }
        auto alloc_type(pool_t& allocator, ref_ast parent, mutability::t mut) {
            return alloc_type(allocator, parent, mut, type_structs::placeholder{});
        }
        ref_symbols make_symbols(pool_t& allocator, ref_symbols parent) {
            return make<symbols_t>::call(allocator, parent);
        }

        util::field med2field(env_t env, median_t elm) {
            auto med = elm.expect<medianc::DECL>();
            auto [name_fin, type_med] = grammar::cursor_helper_t{med.children()}
                                            .tuple_extract<tokc::ID, medianc::TYPE>();
            assert(name_fin && type_med);
            auto name = env.ctx.toks().str(name_fin.value());
            auto type = node2ast::type(env.pass(), type_med.value());
            return {name, type};
        }

        template <typename T>
        struct append_field_span {
            static void append(T& step,
                               pool_t& allocator,
                               ref_ast parent,
                               std::span<const util::field> fs) {
                for (auto& f : fs)
                    append(step, allocator, parent, f);
            }
        };

        template <typename T>
        struct multistep {
            ref_ast ast;
            T val;

            auto& get() {
                return val;
            }
            const auto& get() const {
                return val;
            }

            static multistep<T> make(ref_ast ast) {
                return {ast, T{}};
            }
            static multistep<T> make(ref_ast ast, T&& v) {
                return {ast, std::move(v)};
            }
        };

        template <mutability::t mut = mutability::none()>
        struct type {
            static auto
            alloc(pool_t& allocator, ref_ast parent, type_structs::variant&& d) {
                return alloc_type(allocator, parent, mut, std::move(d));
            }

            static auto alloc(pool_t& allocator, ref_ast parent) {
                return alloc_type(allocator, parent, mut);
            }

            using tupstep = multistep<type_structs::tup_t>;
            struct tup {
                static tupstep begin(ref_ast ast) {
                    return tupstep::make(ast);
                }
                static void append(tupstep& step, ref_type type) {
                    step.val.members.push_back(type);
                }

                template <typename FN>
                    requires std::is_function<FN>::value
                static void append(tupstep& step, FN type_producer) {
                    append(step, type_producer(step.ast));
                }
            };

            using recstep = multistep<type_structs::rec_t>;
            struct rec : append_field_span<recstep> {
                static recstep
                begin(pool_t& allocator, ref_ast ast, ref_symbols ps = nullptr) {
                    return recstep::make(ast, {make_symbols(allocator, ps)});
                }

                static void
                append(recstep& step, pool_t& allocator, ref_ast parent, util::field f) {
                    auto& [name, type] = f;
                    auto dec = alloc_decl<redecl_policy::unique>(allocator,
                                                                 step.ast,
                                                                 step.get().symbols,
                                                                 name.substr())
                                   .value();

                    dec.deref().data = make<decl_structs::rec_member_t>::call(type);
                    step.get().members.emplace_back(dec);
                }
            };

            using varstep = multistep<type_structs::variant_t>;
            struct var : append_field_span<varstep> {
                static varstep begin(pool_t& allocator,
                                     ref_ast parent,
                                     ref_ast ast,
                                     ref_symbols ps = nullptr) {
                    return varstep::make(ast, {make_symbols(allocator, ps)});
                }
                static std::optional<ref_decl>
                append(varstep& step, pool_t& allocator, ref_ast parent, util::field f) {
                    auto& [name, type] = f;
                    if (auto dec = alloc_decl<redecl_policy::unique>(allocator,
                                                                     step.ast,
                                                                     step.get().symbols,
                                                                     name.substr()))

                    {
                        auto v = dec.value();
                        v.deref().data = make<decl_structs::variant_member_t>::call(type);
                        return v;
                    } else {
                        return std::nullopt;
                    }
                }
                static void append(varstep& step,
                                   pool_t& allocator,
                                   ref_ast parent,
                                   std::span<const util::field> fs) {
                    for (auto& f : fs)
                        append(step, allocator, parent, f);
                }
            };

            static ref_type
            fin(pool_t& allocator, ref_ast parent, type_structs::variant&& d) {
                return alloc(allocator, parent, std::move(d));
            }
            static ref_type float16(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::float16_t{});
            }
            static ref_type float32(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::float32_t{});
            }
            static ref_type float64(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::float64_t{});
            }
            static ref_type float128(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::float128_t{});
            }
            static ref_type const_float(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::const_float{});
            }
            static ref_type const_int(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::const_int{});
            }
            static ref_type const_bool(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::const_bool{});
            }
            static ref_type
            uint(pool_t& allocator, ref_ast parent, const std::uint16_t bitsize) {
                return fin(allocator, parent, make<type_structs::uint_t>::call(bitsize));
            }
            static ref_type
            sint(pool_t& allocator, ref_ast parent, const std::uint16_t bitsize) {
                return fin(allocator, parent, make<type_structs::sint_t>::call(bitsize));
            }
            static ref_type
            boolean(pool_t& allocator, ref_ast parent, const std::uint16_t bitsize) {
                return fin(allocator, parent, make<type_structs::bool_t>::call(bitsize));
            }
            static ref_type nulltype(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::void_t{});
            }
            static ref_type placeholder(pool_t& allocator, ref_ast parent) {
                return fin(allocator, parent, type_structs::placeholder{});
            }

            static ref_type record(pool_t& allocator,
                                   ref_ast parent,
                                   std::span<const util::field> fields,
                                   ref_symbols ps = nullptr) {
                auto [ptr, ast] = alloc(allocator, parent);
                auto step = rec::begin(allocator, ast, ps);

                for (auto& f : fields)
                    rec::append(step, allocator, parent, f);

                ptr.deref().data = std::move(step.get());
                return ptr;
            }

            static ref_type variant(pool_t& allocator,
                                    ref_ast parent,
                                    std::span<const util::field> fields,
                                    ref_symbols ps = nullptr) {
                auto [ptr, ast] = alloc(allocator, parent);
                auto step = var::begin(allocator, parent, ast, ps);

                var::append(step, allocator, parent, fields);

                ptr.deref().data = std::move(step.get());
                return ptr;
            }
        };

        using tmut = type<mutability::mut()>;
        using timut = type<mutability::imut()>;
        using tconstant = type<mutability::constant()>;
        using tnone = type<mutability::none()>;

    } // namespace build

    struct ast_printer {
        static void print(ref_ast root) {
            ast_printer printer;
            printer.visit_ast(root);
        }

      private:
        size_t indent_level = 0;

        ast_printer() = default;
        ast_printer(const ast_printer&) = delete;
        ast_printer& operator=(const ast_printer&) = delete;

        static std::string strip_prefix(std::string&& str) {
            const std::vector<std::string> prefixes{
                "semantics::type_structs::",
                "semantics::stmt_structs::",
                "semantics::decl_structs::",
                "semantics::expr_structs::",
            };

            for (const auto& prefix : prefixes) {
                if (str.starts_with(prefix)) {
                    return str.substr(prefix.length());
                }
            }
            return str;
        }

        void print_indent() const {
            std::print("{}", std::string(indent_level * 2, ' '));
        }

        void visit_ast(ref_ast node) {
            if (!node) {
                print_indent();
                std::println("nullptr");
                return;
            }

            print_indent();
            std::print("ast_t({}) ->  ", node.as_void());

            visit(
                node.deref().data,
                [this](std::nullptr_t) { std::println("nullptr"); },
                [this](ref_decl decl) {
                    std::print("decl_t: name='{}', ", decl.deref().name);
                    visit_decl_variant(decl.deref().data);
                },
                [this](ref_type type) {
                    std::print("type_t: mut={}, ", mutability::str(type.deref().mut));
                    visit_type_variant(type.deref().data);
                },
                [this](ref_expr expr) {
                    std::print("expr_t: ");
                    visit_expr_variant(expr.deref().data);
                },
                [this](ref_stmt stmt) {
                    std::print("stmt_t: ");
                    visit_stmt_variant(stmt.deref().data);
                },
                [this](ref_stmts stmts) {
                    std::println("stmts_t: {} elements", stmts.deref().elms.size());
                },
                [this](auto& other) {
                    std::println("unknown: {}", strip_prefix(type_str(other)));
                });

            // Visit children
            indent_level++;
            for (auto& child : node.deref().children) {
                visit_ast(child);
            }
            indent_level--;
        }

        void visit_decl_variant(const decl_structs::variant& data) {
            ovisit(data, [](auto& other) {
                std::println("{}", strip_prefix(type_str(other)));
            });
        }

        void visit_type_variant(const type_structs::variant& data) {
            ovisit(
                data,
                [](const type_structs::indirection& ind) {
                    std::print("indirection: ");
                    visit(
                        ind.data,
                        [](const type_structs::optr_t&) { std::println("optr"); },
                        [](const type_structs::array_t&) { std::println("array"); },
                        [](const type_structs::ptr_t&) { std::println("ptr"); },
                        [](auto&) { std::println("unknown"); });
                },
                [](auto& other) { std::println("{}", strip_prefix(type_str(other))); });
        }

        void visit_expr_variant(const expr_structs::variant& data) {
            visit(
                data,
                [](const expr_structs::operand_t& op) {
                    std::print("operand: ");
                    ovisit(op.data, [](auto& other) {
                        std::println("{}", strip_prefix(type_str(other)));
                    });
                },
                [](const expr_structs::operator_t& op) {
                    std::print("operator: ");
                    visit(
                        op.data,
                        [](const expr_structs::bop_t& v) {
                            std::println("bop:{}", expr_structs::str(v.op.type));
                        },
                        [](const expr_structs::uop_t& v) {
                            std::println("uop:{}", expr_structs::str(v.op.type));
                        },
                        [](auto&) { std::println("unknown"); });
                },
                [](auto& other) { std::println("{}", strip_prefix(type_str(other))); });
        }

        void visit_stmt_variant(const stmt_structs::variant& data) {
            ovisit(data, [](auto& other) {
                std::println("{}", strip_prefix(type_str(other)));
            });
        }
    };

} // namespace semantics
