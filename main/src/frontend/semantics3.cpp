#include "cursor_helper.hpp"
#include "parser.hpp"
#include "semantics3.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <print>
#include <source_location>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

using empty_t = std::monostate;

inline std::string
to_string(const std::source_location &loc = std::source_location::current()) {
    std::ostringstream oss;
    oss << loc.file_name() << ":" << loc.line() << ":" << loc.column() << " in "
        << loc.function_name();
    return oss.str();
}

namespace semantics {

namespace node2ast {

[[nodiscard]] ref_stmts stmts(env_t env, const span_t span);
[[nodiscard]] ref_type type(env_t env, const median_t &med);

struct decl_config {
    bool enable_decl = true;
    bool enable_alias = true;
    bool enable_type_decl = true;
    bool enable_module = true;
    bool enable_module_template = true;

    static consteval decl_config decl_only() {
        return {true, false, false, false, false};
    }
};

template <decl_config config = {}>
[[nodiscard]] ref_decl decl(env_t env, const median_t &med);

void stmts_impl(env_t env, ref_stmts ptr, const span_t span);
[[nodiscard]] ref_expr expr_fn(env_t env, span_t span);
[[nodiscard]] ref_expr expr_fn(env_t env, const median_t &med);

[[nodiscard]] template_inputs_t
template_input(env_t env, median_t &template_inputs_med, ref_symbols symbols);
} // namespace node2ast

[[nodiscard]] std::optional<std::reference_wrapper<template_inputs_t>>
get_template_in_list(decl_structs::module_template_t &val) {
    return val.template_inputs;
}

[[nodiscard]] std::optional<std::reference_wrapper<template_inputs_t>>
get_template_in_list(auto &val) {
    return std::nullopt;
}

[[nodiscard]] std::optional<std::reference_wrapper<template_inputs_t>>
get_template_in_list(ref_decl symbol) {
    return ovisit(symbol.deref().data,
                  [](auto &val) { return get_template_in_list(val); });
}
// At the creation of the pipe we have not finished the init of the expresion
// fully
//  so the check to see if it is the rhs fails
//  thus we need to move this check later in the pipe line
[[nodiscard]] ref_expr find_pipe_lhs_impl(ref_ast current, ref_expr prev) {
    if (!current)
        return nullptr;

    auto &current_val = current.deref();

    return std::visit(
        [&]<typename iT>(iT &cv) -> ref_expr {
            using T = std::remove_cvref_t<iT>;
            if constexpr (cmp_v<T, ref_expr>) {
                auto &expr_val = cv.deref();
                if (const auto op_ptr =
                        expr_val.data.template get_if<expr_structs::operator_t>()) {
                    const auto op = op_ptr.deref();
                    const auto &meta = op.meta();
                    if (meta.op == expr_structs::op_operation_e::PIPE) {
                        const auto &bop =
                            op.data.template unsafe_get<expr_structs::bop_t>();
                        auto return_ctrl = bop.rhs == prev;
                        if (return_ctrl)
                            return bop.lhs;
                    }
                }
                prev = cv;
            } else if constexpr (cmp_v<T, ref_decl>) {
                if (belongs_to_category<decl_structs::callable_cat>(cv.deref().data))
                    return nullptr;
            } else if constexpr (cmp_v<T, std::nullptr_t>) {
                return nullptr;
            }
            return find_pipe_lhs_impl(current.deref().parent, prev);
        },
        current_val.data);
}

ref_expr find_pipe_lhs(ref_expr begin) {
    if (!begin)
        return nullptr;
    auto result = find_pipe_lhs_impl(begin.deref().ast, begin);
    return result;
}

namespace node2ast {

template <typename tag_t>
[[nodiscard]] std::tuple<grammar::cursor_helper_t, ref_decl>
init_proc_decl(env_t env, const median_t &med);

template <typename ResultT>
ref_decl uninitialized_bind(env_t env, const median_t &decl_med) {
    auto [cursor, ptr] = init_proc_decl<std::false_type>(env.pass(), decl_med);
    const auto type_med = cursor.must_extract<medianc::TYPE>();
    auto type_ptr = type(env.with(ptr.deref().ast), type_med);
    ptr.deref().data = ResultT{type_ptr};
    return ptr;
}

[[nodiscard]] template_inputs_t
template_input(env_t env, median_t &template_inputs_med, ref_symbols symbols) {
    template_inputs_t inputs;
    auto cursor = grammar::cursor_helper_t{template_inputs_med.children()};

    while (cursor.within())
        inputs.args.emplace_back([&] {
            auto arg = cursor.must_extract<medianc::ARGUMENT>().fchild().as_median();
            auto cursor = grammar::cursor_helper_t{arg.children()};
            switch (arg.type()) {
            case medianc::TYPE_DECL: {
                auto ptr = alloc_decl<redecl_policy::unique>(
                    env.with(symbols),
                    env.ctx.toks().str(cursor.must_extract<tokc::ID>()));

                auto type_ptr = [&] {
                    auto input_ptr =
                        build::tnone::alloc(env.pool(), ptr.deref().ast, empty_t{});

                    auto init_type_med = cursor.extract<medianc::TYPE>();

                    input_ptr.data.deref().data = type_structs::template_input_t{
                        (init_type_med)
                            ? type(env.with(input_ptr.ast), init_type_med.value())
                            : nullptr};
                    return input_ptr;
                }();

                ptr.deref().data = semantics::decl_structs::type_alias_t{type_ptr};
                return ptr;
            } break;
            case medianc::DECL: {
                return ref_decl{nullptr};
            } break;
            case medianc::MODULE: {
                return ref_decl{nullptr};
            } break;
            default:
                throw std::runtime_error("Unreachable");
            }
        }());

    return inputs;
}

auto deduce_mutability(const auto &const_fin, const auto &mut_fin, const auto &imut_fin) {
    if (const_fin)
        return mutability::constant();
    else if (mut_fin)
        return mutability::mut();
    else if (imut_fin)
        return mutability::imut();
    else
        return mutability::none();
}

// kinda bad but it is what it is
auto extract_mutability(grammar::cursor_helper_t &cursor) {
    auto [const_fin, imut_fin, mut_fin] = cursor.tuple_extract<tokc::BUILTIN_CONSTANT,
                                                               tokc::BUILTIN_IMMUTABLE,
                                                               tokc::BUILTIN_MUTABLE>();
    return deduce_mutability(const_fin, mut_fin, imut_fin);
}

std::uint16_t extract_numeric_bitsize(ctx_t &ctx, const final_t &fin) {

    const auto str = ctx.toks().str(fin).substr(1);
    std::uint16_t v = 0;
    auto [ptr, ec] = std::from_chars(str.begin(), str.end(), v);
    if (ec != std::errc{})
        throw std::invalid_argument("Invalid integer");
    return v;
}

type_structs::variant type_fin(env_t env, ref_type ptr, const final_t fin) {
    auto type = fin->type();
    using namespace type_structs;
    switch (type) {
    case tokc::BUILTIN_VOID:
        return void_t{};

    case tokc::BUILTIN_PTR:
        return make<optr_t>::call();

    case tokc::TYPE_FLOAT:
        switch (extract_numeric_bitsize(env.ctx, fin)) {
        case 16:
            return float16_t{};
        case 32:
            return float32_t{};
        case 64:
            return float64_t{};
        case 128:
            return float128_t{};
        default:
            throw std::runtime_error("Unknown float size");
        }
    case tokc::TYPE_INT:
        return make<sint_t>::call(extract_numeric_bitsize(env.ctx, fin));
    case tokc::TYPE_UINT:
        return make<uint_t>::call(extract_numeric_bitsize(env.ctx, fin));
    case tokc::TYPE_BOOLEAN:
        return make<bool_t>::call(extract_numeric_bitsize(env.ctx, fin));

    default:
        throw std::runtime_error("type final path, unknown final");
    }
}

ref_type type_arg(env_t env, const median_t &med) { return type(env.pass(), med); }

ref_decl decl_arg(env_t env, const median_t &med) {
    return uninitialized_bind<decl_structs::argument_t>(env.pass(), med);
}

enum class fnsig_mode { FNDECL, FNTEMPLATE, FNTYPE };
template <fnsig_mode mode>
[[nodiscard]] auto fnsig(env_t env, grammar::cursor_helper_t &cursor) {
    constexpr bool is_template = mode == fnsig_mode::FNTEMPLATE;
    constexpr bool is_type = mode == fnsig_mode::FNTYPE;
    constexpr bool is_decl = mode == fnsig_mode::FNDECL || mode == fnsig_mode::FNTEMPLATE;

    struct {
        std::optional<median_t> state_med, template_med, args_med, ret_med;
    } meds;

    if constexpr (is_decl) {
        std::tie(meds.state_med, meds.template_med) =
            cursor
                .tuple_extract<medianc::FN_STATE_LIST, medianc::TEMPLATE_ARGUMENT_LIST>();
    }

    std::tie(meds.args_med, meds.ret_med) =
        cursor.tuple_extract<medianc::FN_ARGS, medianc::FN_RET>();

    if constexpr (is_decl) {
        const bool is_closure = [&meds] {
            if (meds.state_med) [[unlikely]]
                return (meds.state_med->len() > 0);
            return false;
        }();
        assert(!is_closure);
    }

    ref_symbols fnsymbols = [&env] -> auto {
        if constexpr (mode != fnsig_mode::FNTYPE)
            return make<symbols_t>::call(env.pool(), env.symbols());
        else
            return env.symbols();
    }();

    if constexpr (is_decl) {
        if (meds.state_med) [[unlikely]]
            throw std::runtime_error("We do not support state lists");
    }

    using ArgT = std::conditional_t<mode == fnsig_mode::FNTYPE, type_t, decl_t>;
    constexpr auto arg_fn = [](env_t env, const median_t &med) -> auto {
        if constexpr (is_type) {
            med.expect<medianc::TYPE>();
            return type_arg(env.pass(), med);
        } else if constexpr (is_decl || is_template) {
            med.expect<medianc::DECL>();
            return decl_arg(env.pass(), med);
        }
    };

    vector<ref<ArgT>> args;
    if (meds.args_med) [[likely]] {
        auto cursor = grammar::cursor_helper_t{meds.args_med->children()};
        while (cursor.within()) {
            const auto emed =
                cursor.must_extract<medianc::ARGUMENT>().fchild().as_median();
            auto ptr = arg_fn(env.with(fnsymbols), emed);
            args.push_back(ptr);
        }
    }

    ref_type ret =
        type(env.pass(), grammar::expect<medianc::TYPE>(meds.ret_med->fchild().node()));

    if constexpr (mode == fnsig_mode::FNTYPE) {
        return type_structs::fntype_t{std::move(args), ret};
    } else {
        return fnsig_t{fnsymbols, ret, std::move(args)};
    }
}

namespace indirection {
auto array_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto [len_med, type_med] =
        cursor.tuple_extract<medianc::ARRAY_LENGTH, medianc::TYPE>();

    auto len_expr = [&] -> ref_expr {
        auto len_ch = len_med->children();
        auto m = grammar::expect<medianc::EXPR>(len_ch.at(0)->node());
        if (len_ch.size2() > 0)
            return expr_fn(env.pass(), m);
        auto array_operand =
            make<expr_structs::operand_t>::call(expr_structs::infered_t{});

        return alloc_expr(env.pool(),
                          env.parent(),
                          std::move(array_operand),
                          [](auto &alloc, ref_ast ast) -> ref_type {
                              return build::tnone::placeholder(alloc, ast);
                          });
    }();

    auto type_ptr = type(env.pass(), type_med.value());

    return make<type_structs::array_t>::call(len_expr, type_ptr);
}
template <bool is_mut> auto ptr_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto type_med = cursor.must_extract<medianc::TYPE>();
    auto type_ptr = type(env.pass(), type_med);
    return make<type_structs::ptr_t>::call((is_mut) ? type_structs::ptr_mut::mut()
                                                    : type_structs::ptr_mut::imut(),
                                           type_ptr);
}
} // namespace indirection

auto typeof_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto m = med.fchild().as_median();
    auto expr_ptr = expr_fn(env.with(env.ctx.spool()), m);
    return type_structs::typeof_t{expr_ptr};
}

type_structs::variant chain_fn(env_t env, ref_type ptr, const median_t &chain_med) {
    return type_structs::incomplete::alias{env.symbols(), chain_med};
}

type_structs::fntemplate_t fn_path_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    return {fnsig<fnsig_mode::FNTEMPLATE>(env.pass(), cursor)};
}
type_structs::fntype_t fntype_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    return fnsig<fnsig_mode::FNTYPE>(env.pass(), cursor);
}

type_structs::tup_t tup_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    type_structs::tup_t vec;
    while (cursor.within()) {
        const auto elm = grammar::expect<medianc::TYPE>(
            cursor.must_extract<medianc::ELEMENT>().fchild().node());
        auto entry = type(env.pass(), elm);
        vec.members.emplace_back(entry);
    }

    return vec;
}

type_structs::variant_t variant_fn(env_t env, const median_t &med) {
    auto body_med =
        grammar::cursor_helper_t{med.children()}.must_extract<medianc::BODY>();
    using b = build::tnone::var;
    auto step = b::begin(env.pool(), env.parent(), env.parent());

    auto ch = body_med.children();
    auto cursor = grammar::cursor_helper_t{ch};
    while (cursor.within()) {
        const auto elm = cursor.must_extract<medianc::ELEMENT>();
        auto med = elm.fchild().as_median().expect<medianc::DECL>();
        b::append(step, env.pool(), env.parent(), build::med2field(env.pass(), med));
    }
    return step.val;
}

type_structs::rec_t rec_fn(env_t env, const median_t &med) {
    auto body_med =
        grammar::cursor_helper_t{med.children()}.must_extract<medianc::BODY>();
    auto ch = body_med.children();

    using b = build::tnone::rec;
    auto step = b::begin(env.pool(), env.parent(), env.symbols());
    auto cursor = grammar::cursor_helper_t{ch};
    while (cursor.within()) {
        const auto elm = cursor.must_extract<medianc::ELEMENT>();
        auto med = elm.fchild().as_median().expect<medianc::DECL>();
        b::append(step, env.pool(), env.parent(), build::med2field(env.pass(), med));
    }
    return step.val;
}

type_structs::variant type_median(env_t env, ref_type ptr, const median_t &med) {
    switch (med.type()) {
    case medianc::CHAIN:
        return chain_fn(env.pass(), ptr, med);
    case medianc::PTR:
        return indirection::ptr_fn<true>(env.pass(), med);
    case medianc::IMMUTABLE_PTR:
        return indirection::ptr_fn<false>(env.pass(), med);
    case medianc::ARRAY:
        return indirection::array_fn(env.pass(), med);
    case medianc::INFER:
        return type_structs::infered_t{};
    case medianc::TYPEOF:
        return typeof_fn(env.pass(), med);
    case medianc::FN_TEMPLATE:
        return fn_path_fn(env.pass(), med);
    case medianc::FN_TYPE:
        return fntype_fn(env.pass(), med);
    case medianc::TUPLE:
        return tup_fn(env.pass(), med);

    case medianc::RECORD:
        return rec_fn(env.pass(), med);
    case medianc::VARIANT:
        return variant_fn(env.pass(), med);
    default:
        throw std::runtime_error("type median path, unknown median");
    }
}

type_structs::variant
type_path(env_t env, ref_type ptr, grammar::cursor_helper_t cursor) {
    auto data = [&] {
        auto [fin, med] = cursor.tuple_extract<tokc::any, medianc::any>();
        if (fin)
            return type_fin(env.pass(), ptr, fin.value());
        else if (med)
            return type_median(env.pass(), ptr, med.value());
        else [[unlikely]]
            throw std::runtime_error("There should always be a type");
    }();

    return data;
}

[[nodiscard]] ref_type type(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto mut = extract_mutability(cursor);

    auto [ptr, ast] = make<type_t>::call(env.ctx.pool(), env.parent(), mut, empty_t{});

    ptr.deref().data = type_path(env.with(ast), ptr, cursor);
    return ptr;
}

namespace decl_structs {
using namespace ::semantics::decl_structs;

[[nodiscard]] decl_structs::variant
ambiguous_decl(env_t env, ref_decl ptr, grammar::cursor_helper_t cursor) {
    auto [type_med, val_med] = cursor.tuple_extract<medianc::TYPE, medianc::VALUE>();

    auto type_ptr = type(env.pass(), type_med.value());
    std::optional<ref_expr> expr_ptr =
        (val_med) ? expr_fn(env.pass(), val_med.value().fchild().as_median().children())
                  : std::optional<ref_expr>(std::nullopt);
    return incomplete::bind{env.symbols(), type_ptr, expr_ptr};
}

[[nodiscard]] decl_structs::variant
decl_alias(env_t env, ref_decl ptr, grammar::cursor_helper_t cursor) {
    auto med = cursor.must_extract<medianc::CHAIN>();
    return incomplete::alias{env.symbols(), med};
}

[[nodiscard]] type_alias_t type_alias(env_t env, grammar::cursor_helper_t cursor) {
    auto type_med = cursor.must_extract<medianc::TYPE>();
    auto type_ptr = type(env.pass(), type_med);
    return {type_ptr};
}

auto module_meds(grammar::cursor_helper_t &cursor) {
    return cursor.must_extract<medianc::BODY>();
}

void reuse_module(env_t env, grammar::cursor_helper_t cursor, module_t &existing) {
    auto body_med = module_meds(cursor);
    stmts_impl(env.with(existing.frame.stmts.deref().ast, existing.frame.symbols),
               existing.frame.stmts,
               body_med.children());
}

[[nodiscard]] module_t new_module(env_t env, grammar::cursor_helper_t cursor) {
    auto body_med = module_meds(cursor);
    auto symbols = make<symbols_t>::call(env.ctx.pool(), env.symbols());
    return {symbols, stmts(env.with(symbols), body_med.children())};
}

module_template_t new_module_template(env_t env, grammar::cursor_helper_t cursor) {
    auto [template_inputs_med, body_med] =
        cursor.tuple_extract<medianc::TEMPLATE_ARGUMENT_LIST, medianc::BODY>();
    if (!body_med)
        throw std::runtime_error("Expected to have a body");

    auto symbols = make<symbols_t>::call(env.ctx.pool(), env.symbols());

    template_inputs_t template_inputs = [&] {
        if (template_inputs_med) [[likely]] {
            return template_input(env.pass(), template_inputs_med.value(), symbols);
        } else {
            throw std::runtime_error(
                "If you do not use template inputs use a scope not a pattern");
        }
    }();
    return {{symbols, stmts(env.with(symbols), body_med->children())},
            std::move(template_inputs)};
}

} // namespace decl_structs

template <typename tag_t>
[[nodiscard]] std::tuple<grammar::cursor_helper_t, ref_decl>
init_proc_decl(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto name = env.ctx.toks().str(cursor.must_extract<tokc::ID>());

    constexpr redecl_policy rpolicy = cmp_v<tag_t, decl_structs::module_t>
                                          ? redecl_policy::reuse
                                          : redecl_policy::unique;
    auto ptr = alloc_decl<rpolicy>(env.pass(), std::move(name));

    return std::make_tuple(std::move(cursor), std::move(ptr));
}

template <auto new_fn>
[[nodiscard]] ref_decl ambiguous_decl_path(env_t env, const median_t &med) {
    auto [cursor, ptr] = init_proc_decl<std::false_type>(env.pass(), med);
    ptr.deref().data = new_fn(env.with(ptr.deref().ast), ptr, cursor);
    return ptr;
}

template <auto new_fn> [[nodiscard]] ref_decl decl_path(env_t env, const median_t &med) {
    auto [cursor, ptr] = init_proc_decl<std::false_type>(env.pass(), med);
    ptr.deref().data = {new_fn(env.with(ptr.deref().ast), cursor)};
    return ptr;
}

template <auto reuse_fn, auto new_fn, typename localy_indistinct>
[[nodiscard]] ref_decl decl_path(env_t env, const median_t &med) {
    auto [cursor, ptr] = init_proc_decl<localy_indistinct>(env.pass(), med);

    auto var_data = ptr.deref().data;
    if (auto data = var_data.template get_if<localy_indistinct>()) {
        reuse_fn(env.with(ptr.deref().ast), cursor, data.deref());
    } else if (var_data.template has<empty_t>()) {
        ptr.deref().data = {new_fn(env.with(ptr.deref().ast), cursor)};
    } else {
        throw std::runtime_error("decl_path: unexpected variant in "
                                 "decl_t::data — neither empty_t nor "
                                 "expected localy_indistinct type");
    }
    return ptr;
}

[[nodiscard]] auto handle_decl(env_t env, const median_t &med) {
    return ambiguous_decl_path<decl_structs::ambiguous_decl>(env.pass(), med);
}

[[nodiscard]] auto handle_alias(env_t env, const median_t &med) {
    return ambiguous_decl_path<decl_structs::decl_alias>(env.pass(), med);
}

[[nodiscard]] auto handle_type_decl(env_t env, const median_t &med) {
    return decl_path<decl_structs::type_alias>(env.pass(), med);
}

[[nodiscard]] auto handle_module(env_t env, const median_t &med) {
    return decl_path<decl_structs::reuse_module,
                     decl_structs::new_module,
                     semantics::decl_structs::module_t>(env.pass(), med);
}

[[nodiscard]] auto handle_module_template(env_t env, const median_t &med) {
    return decl_path<decl_structs::new_module_template>(env.pass(), med);
}

template <decl_config conf> [[nodiscard]] ref_decl decl(env_t env, const median_t &med) {
    switch (med.type()) {
    case medianc::DECL:
        if constexpr (conf.enable_decl)
            return handle_decl(env.pass(), med);
        break;

    case medianc::ALIAS:
        if constexpr (conf.enable_alias)
            return handle_alias(env.pass(), med);
        break;

    case medianc::TYPE_DECL:
        if constexpr (conf.enable_type_decl)
            return handle_type_decl(env.pass(), med);
        break;

    case medianc::MODULE:
        if constexpr (conf.enable_module)
            return handle_module(env.pass(), med);
        break;

    case medianc::MODULE_TEMPLATE:
        if constexpr (conf.enable_module_template)
            return handle_module_template(env.pass(), med);
        break;

    default:
        break;
    }

    throw std::runtime_error("Unsupported or disabled declaration median type");
}

[[nodiscard]] expr_structs::operator_t tokc_to_op(const tokc::e token) {
    using namespace expr_structs;
    switch (token) {
    case tokc::MINUSMINUS:
        return {make_uop(op_operation_e::MINUSMINUS, nullptr, {})};
    case tokc::PLUSPLUS:
        return {make_uop(op_operation_e::PLUSPLUS, nullptr, {})};
    case tokc::GREATER:
        return {make_bop(op_operation_e::GREATER, nullptr, nullptr)};
    case tokc::PLUS:
        return {make_bop(op_operation_e::PLUS, nullptr, nullptr)};
    case tokc::ASIGN:
        return {make_bop(op_operation_e::ASSIGN, nullptr, nullptr)};
    case tokc::DIAMOND:
        return {make_bop(op_operation_e::DIAMOND, nullptr, nullptr)};
    case tokc::MUL:
        return {make_bop(op_operation_e::MULT, nullptr, nullptr)};
    case tokc::OR:
        return {make_bop(op_operation_e::OR, nullptr, nullptr)};
    case tokc::MINUS:
        return {make_bop(op_operation_e::MINUS, nullptr, nullptr)};
    case tokc::MULASIGN:
        return {make_bop(op_operation_e::MULTASSIGN, nullptr, nullptr)};
    case tokc::AND:
        return {make_bop(op_operation_e::AND, nullptr, nullptr)};
    case tokc::LEQUALS:
        return {make_bop(op_operation_e::LEQ, nullptr, nullptr)};
    case tokc::MINUSGREATER:
        return {make_bop(op_operation_e::PIPE, nullptr, nullptr)};
    case tokc::LESS:
        return {make_bop(op_operation_e::LESS, nullptr, nullptr)};
    case tokc::MINUSASIGN:
        return {make_bop(op_operation_e::MINUSASSIGN, nullptr, nullptr)};
    case tokc::LESSLESS:
        return {make_bop(op_operation_e::SLEFT, nullptr, nullptr)};
    case tokc::XOR:
        return {make_bop(op_operation_e::XOR, nullptr, nullptr)};
    case tokc::AMPERSAND:
        return {make_uop(op_operation_e::ADDRESS, nullptr, {})};
    case tokc::EMARK:
        return {make_uop(op_operation_e::NOT, nullptr, {})};
    case tokc::MODULO:
        return {make_bop(op_operation_e::MOD, nullptr, nullptr)};
    case tokc::EQUALS:
        return {make_bop(op_operation_e::EQ, nullptr, nullptr)};
    case tokc::GEQUALS:
        return {make_bop(op_operation_e::GEQ, nullptr, nullptr)};
    case tokc::DIVASIGN:
        return {make_bop(op_operation_e::DIVASSIGN, nullptr, nullptr)};
    case tokc::GREATERGREATER:
        return {make_bop(op_operation_e::SRIGHT, nullptr, nullptr)};
    case tokc::PLUSASIGN:
        return {make_bop(op_operation_e::PLUSASSIGN, nullptr, nullptr)};
    case tokc::EMARKEQUALS:
        return {make_bop(op_operation_e::NEQ, nullptr, nullptr)};
    case tokc::DIV:
        return {make_bop(op_operation_e::DIV, nullptr, nullptr)};
    default:
        [[unlikely]] throw std::runtime_error("This token is not an operator, " +
                                              std::string(tokc::str(token)));
    }
}

expr_structs::uop_t::as_payload_t as_payload_fn(env_t env, const median_t &med) {
    auto type_med = grammar::expect<medianc::TYPE>(med.fchild().node());
    return expr_structs::uop_t::as_payload_t{type(env.pass(), type_med)};
}

[[nodiscard]] ref_expr operator_fn(env_t env, const median_t &med) {
    auto node = med.fchild().node();
    using namespace expr_structs;
    auto ptr = alloc_expr(env.pool(),
                          env.parent(),
                          std::monostate{},
                          [](auto &alloc, ref_ast ast) -> ref_type {
                              return build::tnone::placeholder(alloc, ast);
                          });

    auto base = ovisit(
        node,
        [](const final_t &val) -> operator_t { return tokc_to_op(val->type()); },
        [&env, ptr](const median_t &val) -> operator_t {
            switch (val.type()) {
            case medianc::AS: {
                auto as_payload = as_payload_fn(env.with(ptr.deref().ast), val);
                return {
                    make_uop(op_operation_e::AS, nullptr, uop_t::payload_t{as_payload})};
            }
            default:
                std::unreachable();
            }
        },
        [](const auto &) -> operator_t { std::unreachable(); });

    ptr.deref().data = std::move(base);

    // auto& data =
    //     ptr.deref().data.get<expr_structs::operator_t>().data.get_base();

    return ptr;
}

expr_structs::block_t block_fn(env_t env, const median_t &med) {
    auto frame_symbols = make<symbols_t>::call(env.pool(), env.symbols());

    auto body_med =
        grammar::cursor_helper_t{med.children()}.must_extract<medianc::BODY>();
    auto frame_stmts = stmts(env.with(frame_symbols), body_med.children());

    return {util::frame{frame_symbols, frame_stmts}, nullptr};
}

expr_structs::initlist_t initlist(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{
        grammar::expect<medianc::BODY>(med.fchild().node()).children()};

    expr_structs::initlist_t vals;
    while (cursor.within()) {
        auto med = cursor.must_extract<medianc::EXPR>();
        auto ptr = expr_fn(env.pass(), med);
        vals.data.emplace_back(ptr);
    }
    return vals;
}

expr_structs::pipe_t operand_pipe_fn(env_t env, ref_expr ptr, const median_t &med) {
    return expr_structs::pipe_t{nullptr};
}

expr_structs::if_t if_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto val = expr_structs::if_t{{}};
    bool one_else = false;
    while (cursor.within()) {
        auto var_med = cursor.extract<medianc::any>();
        if (!var_med) [[unlikely]] {
            std::unreachable();
            // throw std::runtime_error(std::string(__PRETTY_FUNCTION__)
            //                          + " This shouldn't happen");
        }
        if (var_med->type() == medianc::IF) {
            auto cursor = grammar::cursor_helper_t{var_med->children()};
            auto [ctrl_expr_med, body_med] =
                cursor.tuple_extract<medianc::CTRL_EXPR, medianc::BODY>();

            auto expr = expr_fn(env.pass(), ctrl_expr_med->fchild().as_median());
            auto stmts_ptr = stmts(env.pass(), body_med->children());
            auto elif = expr_structs::internal_if{expr, stmts_ptr};

            val.ifs.emplace_back(std::move(elif));
        } else if (var_med->type() == medianc::ELSE) {
            {
                if (cursor.within()) [[unlikely]]
                    throw std::runtime_error(
                        "The else part of the if expr should be last");
                if (one_else) [[unlikely]]
                    throw std::runtime_error(
                        "Can't have more than one else in a if expr");
            }
            one_else = true;
            auto body_med =
                grammar::cursor_helper_t{var_med->children()}.extract<medianc::BODY>();
            auto el = expr_structs::internal_if{{nullptr},
                                                stmts(env.pass(), body_med->children())};
            ;
            val.ifs.push_back({el});
        } else {
            std::unreachable();
            // throw std::runtime_error(std::string(__PRETTY_FUNCTION__)
            //                          + "This shouldn't happen");
        }
    }
    return val;
}

expr_structs::operand_variants sizeof_path(env_t env, const median_t &med) {
    auto fch = med.fchild().as_median();
    switch (fch.type()) {
    case medianc::TYPE:
        return expr_structs::sizeof_type_t{type(env.pass(), fch)};
    case medianc::EXPR:
        return expr_structs::sizeof_expr_t{expr_fn(env.pass(), fch.children())};
    default:
        std::unreachable();
    }
}

expr_structs::variant_init_t variant_init_fn(env_t env, const median_t &med) {
    auto cursor = grammar::cursor_helper_t{med.children()};
    auto [field_name_fin, init_expr_med] =
        cursor.tuple_extract<tokc::ID, medianc::EXPR>();

    assert(field_name_fin && init_expr_med);

    auto name = env.ctx.toks().str(field_name_fin.value());
    auto expr_ptr = expr_fn(env.pass(), init_expr_med.value().children());
    return {name, expr_ptr};
}

expr_structs::operand_variants
operand_med_fn(env_t env, ref_expr ptr, const median_t &med) {
    switch (med.type()) {
    case medianc::CHAIN:
        return expr_structs::incomplete::chain{env.symbols(), med};
    case medianc::BLOCK_EXPR:
        return block_fn(env.pass(), med);
    case medianc::SIZEOF:
        return sizeof_path(env.pass(), med);
    case medianc::COMPOUND_LITERAL:
        throw std::runtime_error("Compounds are not supported yet");
    case medianc::FN_LITERAL:
        throw std::runtime_error("Function literals/lambdas are not supported yet");
    case medianc::IF_EXPR:
        return if_fn(env.pass(), med);
    case medianc::PIPE:
        return operand_pipe_fn(env.pass(), ptr, med);
    case medianc::VARIANT_INIT:
        return variant_init_fn(env.pass(), med);
    case medianc::INIT_LIST:
        return initlist(env.pass(), med);
    default:
        throw std::runtime_error(std::string(__PRETTY_FUNCTION__) + "Unknown median " +
                                 std::string(medianc::str(med.type())));
    }
}

ref_expr operand_fn(env_t env, median_t med) {
    auto node = med.children().begin()->node();
    using namespace expr_structs;
    auto ptr = alloc_expr(
        env.pool(), env.parent(), empty_t{}, [](auto &alloc, ref_ast ast) -> ref_type {
            return build::tnone::placeholder(alloc, ast);
        });
    auto base = ovisit(
        node,
        [&env](const final_t &val) -> operand_t {
            switch (val->type()) {
            case tokc::INT: {
                auto sv = env.ctx.toks().str(val);
                return {int_t{llvm::APInt{64, strview_num<std::uint64_t>(sv)}}};
            }
            case tokc::FLOAT: {
                auto sv = env.ctx.toks().str(val);
                return {float_t{llvm::APFloat{llvm::APFloat::IEEEquad(), sv}}};
            }
            case tokc::BUILTIN_TRUE:
                return {boolean_t{true}};
            case tokc::BUILTIN_FALSE:
                return {boolean_t{false}};
            case tokc::BUILTIN_NULL:
                return {null_t{}};
            default:
                std::unreachable();
            }
        },
        [&ptr, &env](const median_t &val) -> operand_t {
            return {operand_med_fn(env.with(ptr.deref().ast), ptr.data, val)};
        },
        [](const auto &val) -> operand_t { std::unreachable(); });
    ptr.deref().data = std::move(base);
    return ptr.data;
}

ref_expr pratt_parsing(env_t env, span_t ch, cursor_t &cursor, size_t min_prec) {
    if (!ch.contains(cursor)) [[unlikely]]
        throw std::runtime_error("Expected to have more nodes");

    ref_expr lhs = nullptr;
    const auto med = cursor++->as_median();
    switch (med.type()) {
    case medianc::OPERATOR: {
        auto op_ptr = operator_fn(env.pass(), med);
        auto &op = op_ptr.deref().data.expect<expr_structs::operator_t>();
        // This is required to have stuff like
        //  negative -val
        //  positive +val
        //  ++val and val++ etc...
        //  if (op->meta() != expr_s::op_pos_e::PREFIX) {
        //    operator_s::prefix_fallback(op);
        //  }
        const auto &meta = op.meta();
        if (auto uop = op_ptr.deref()
                           .data.get<expr_structs::operator_t>()
                           .data.get_if<expr_structs::uop_t>()) [[likely]] {
            // this causes the + not passing since it set's it too high
            uop.deref().operand =
                pratt_parsing(env.with(op_ptr.deref().ast), ch, cursor, meta.prec);
            lhs = op_ptr;
        } else {
            std::unreachable();
            // throw std::runtime_error("This should be a unary operator");
        }
        break;
    }
    case medianc::OPERAND: {
        lhs = operand_fn(env.pass(), med);
        break;
    }
    default:
        std::unreachable();
    }

    while (ch.contains(cursor)) {
        const auto med = cursor->as_median();
        if (med.type() != medianc::OPERATOR)
            break;
        const auto op = med.fchild().as_final();
        auto tok = tokc_to_op(op.type());
        const expr_structs::op_meta_t meta = tok.meta();
        /* if (med.fchild().is_final()) {
            auto op = med.fchild().as_final();
            auto tok = token_to_operator(op.type());
            meta = tok.meta();
        } else {
            throw std::runtime_error("Due to a memory leak caused by the "
                                     "nature of this loop, median "
                                     "operators are not supported fully yet");
        } */

        if (meta == expr_structs::op_pos_e::PREFIX) [[unlikely]] {
            throw std::runtime_error("Expected Postfix or Infix operator");
        } else if (meta == expr_structs::op_pos_e::POSTFIX) {
            auto lprec = left_bp(meta);
            if (lprec < min_prec)
                break;
            auto op_ptr = operator_fn(env.pass(), med);
            auto &op = op_ptr.deref().data.get<expr_structs::operator_t>();
            auto &val = op.data.get<expr_structs::uop_t>();
            cursor.advance();
            transfer_ast(op_ptr.deref().ast, lhs.deref().ast);
            val.operand = lhs;
            lhs = op_ptr;
            continue;
        } else if (meta == expr_structs::op_pos_e::INFIX) {
            auto lprec = left_bp(meta);
            auto rprec = right_bp(meta);
            if (lprec < min_prec)
                break;

            // and this
            auto op_ptr = operator_fn(env.pass(), med);
            auto &op = op_ptr.deref().data.get<expr_structs::operator_t>();
            auto &op_val = op.data.get<expr_structs::bop_t>();
            cursor.advance();

            ref_expr rhs = pratt_parsing(env.with(op_ptr.deref().ast), ch, cursor, rprec);

            transfer_ast(op_ptr.deref().ast, lhs.deref().ast);

            op_val.lhs = lhs;
            op_val.rhs = rhs;
            lhs = op_ptr;
            continue;
        }
        break;
    }
    return lhs;
}

ref_expr expr_tree(env_t env, span_t ch) {
    auto cursor = ch.begin();
    auto ptr = pratt_parsing(env.pass(), ch, cursor, 0);
    if (ch.contains(cursor))
        throw std::runtime_error("Did not consume the whole expresion");

    return ptr;
}

[[nodiscard]] ref_expr expr_fn(env_t env, span_t span) {
    auto ptr = expr_tree(env.pass(), span);
    return ptr;
}

[[nodiscard]] ref_expr expr_fn(env_t env, const median_t &med) {
    return expr_tree(env.pass(), med.expect<medianc::EXPR>().children());
}

template <typename T> auto exit_ctrl_fn(env_t env, const median_t &med) {
    auto expr_med = grammar::cursor_helper_t{med.children()}.extract<medianc::EXPR>();
    return T{(expr_med) ? expr_fn(env.pass(), expr_med.value()) : nullptr};
}

auto loop_fn(env_t env, const median_t &med) {
    auto ch = med.children();
    auto cursor = grammar::cursor_helper_t{ch};

    auto [decls_med, ctrl_expr_med, iter_med, body_med] =
        cursor.tuple_extract<medianc::LOOP_DECL,
                             medianc::CTRL_EXPR,
                             medianc::LOOP_ITER,
                             medianc::BODY>();

    auto symbols = make<symbols_t>::call(env.pool(), env.symbols());

    if (decls_med) {
        auto ch = decls_med.value().children();
        auto cursor = grammar::cursor_helper_t(ch);

        while (cursor.within()) {
            auto decl_med = cursor.must_extract<medianc::DECL>();
            auto decl_ptr = decl<decl_config::decl_only()>(env.with(symbols), decl_med);
            (void)decl_ptr;
        }
    }

    ref_expr ctrl_expr;
    {
        if (!ctrl_expr_med)
            throw std::runtime_error("Expected to have a ctrl expresion");
        auto expr_med = ctrl_expr_med->fchild().as_median();
        ctrl_expr = expr_fn(env.with(symbols), expr_med.children());
    }

    std::optional<ref_expr> iter_expr;
    if (iter_med) {
        const auto &val = iter_med.value().fchild().as_median();
        iter_expr = expr_fn(env.with(symbols), val);
    }

    assert(body_med);
    auto body_expr =
        expr_fn(env.with(symbols), body_med->fchild().as_median().children());

    return stmt_structs::loop_t{
        symbols, ctrl_expr, iter_expr.value_or(nullptr), body_expr};
}

[[nodiscard]] stmt_structs::variant
stmt_path(env_t env, ref_stmt ptr, const median_t med) {
    switch (med.type()) {
    case medianc::DECL:
    case medianc::FN_DECL:
    case medianc::TYPE_DECL:
    case medianc::ALIAS:
    case medianc::MODULE_TEMPLATE:
    case medianc::MODULE:
        return stmt_structs::decl{decl(env.pass(), med)};
    case medianc::EXPR:
        return stmt_structs::expr{expr_fn(env.pass(), med.children())};
    case medianc::BECOME:
        return exit_ctrl_fn<stmt_structs::become_t>(env.pass(), med);
    case medianc::RETURN:
        return exit_ctrl_fn<stmt_structs::return_t>(env.pass(), med);
    case medianc::BREAK:
        return exit_ctrl_fn<stmt_structs::break_t>(env.pass(), med);
    case medianc::LOOP:
        return loop_fn(env.pass(), med);
    // case medianc::UNWRAP_DECL:
    //     return unwrap_fn(env.pass(), ptr, med);
    case medianc::IMPORT:
    default:
        throw std::runtime_error("Unknown stmt median type");
        break;
    }
}

[[nodiscard]] ref_stmt stmt(env_t env, const median_t med) {
    auto ptr = make<stmt_t>::call(env.ctx.pool(), env.parent());
    ptr.data.deref().data = stmt_path(env.with(ptr.ast), ptr, med);
    return ptr.data;
}

void stmts_impl(env_t env, ref_stmts ptr, const span_t span) {
    for (auto &elm : span) {
        auto &fc = elm.as_median().fchild();
        auto val = [&] -> ref_stmt {
            if (fc.is_final()) {
                auto &fin = fc.as_final();
                switch (fin->type()) {
                case tokc::BUILTIN_UNREACHABLE: {
                    auto ptr = make<stmt_t>::call(env.ctx.pool(), env.parent());
                    ptr.data.deref().data = stmt_structs::unreachable{};
                    return ptr;
                }
                default:
                    std::unreachable();
                }
            } else if (fc.is_median()) {
                return stmt(env.with(ptr.deref().ast), fc.as_median());
            } else {
                std::unreachable();
            }
        }();
        ptr.deref().elms.emplace_back(val);
    }
};

[[nodiscard]] ref_stmts stmts(env_t env, ref_stmts ptr, const span_t span) {
    stmts_impl(env.with(ptr.deref().ast), ptr, span);
    return ptr.data;
}

[[nodiscard]] ref_stmts stmts(env_t env, const span_t span) {
    auto ptr = make<stmts_t>::call(env.ctx.pool(), env.parent());
    return stmts(env.with(ptr.ast), ptr.data, span);
}

} // namespace node2ast
} // namespace semantics

namespace semantics {

struct ast_traversal_base {
    virtual void visit(ref_decl &) {}
    virtual void visit(ref_expr &) {}
    virtual void visit(ref_type &) {}
    virtual void visit(ref_stmt &) {}
    virtual void visit(ref_stmts &) {}

    virtual void visit(const std::nullptr_t &) {}
    virtual void visit(const std::monostate &) {}

    void dispatch(ref_ast &ptr) {
        ::visit(
            ptr.deref().data,
            [this](std::monostate &v) { this->visit(v); },
            [this](std::nullptr_t &v) { this->visit(v); },
            [this](ref_decl &v) { this->visit(v); },
            [this](ref_expr &v) { this->visit(v); },
            [this](ref_type &v) { this->visit(v); },
            [this](ref_stmt &v) { this->visit(v); },
            [this](ref_stmts &v) { this->visit(v); },
            [](auto &) {});
    }

  public:
    virtual void visit_ast(ref_ast &ptr) final {
        dispatch(ptr);

        for (auto &child : ptr.deref().children)
            visit_ast(child);
    }

    virtual ~ast_traversal_base() = default;
};

bool is_incomplete(ref_decl ptr) {
    return belongs_to_category<decl_structs::incomplete_cat>(ptr.deref().data);
}
bool is_incomplete(ref_type ptr) {
    return ptr.deref().data.has<type_structs::incomplete::alias>();
}
// bool is_incomplete(ref_expr ptr) {
//     return ptr.deref().data.has<expr_structs::incomplete::chain>();
// }

// How do you make the functions
// How do you resolve expresion types
// How do you resolve expresions
struct resolve_pass {
    ctx_t &ctx;

  private:
    struct resolver {
        using elm = ref_decl;
        using result_t = vector<elm>;

        ref_ast caller;
        result_t result;

      private:
        static consteval auto get_lookup_fn(bool cond) -> auto {
            if (cond)
                return symbols_t::ancestor_lookup;
            else
                return symbols_t::local_lookup;
        }

        void handle_incomplete(ctx_t &ctx, ref_decl ptr) {
            auto &deref = ptr.deref();
            if (auto v = deref.data.get_if<decl_structs::incomplete::alias>()) {
                handle(ctx, ptr, v.deref());
            } else if (auto v = deref.data.get_if<decl_structs::incomplete::bind>()) {
                handle(ctx, ptr, v.deref());
            } else [[unlikely]] {
                std::println("False call");
            }
        }

        template <bool /*is_first_call*/>
        bool process_template_init(ctx_t &ctx,
                                   ref_symbols symbols,
                                   grammar::cursor_helper_t &cursor) {
            if (auto med = cursor.extract<medianc::TEMPLATE_INSTATIATION>()) {
                auto prev = result.back();
                if (auto module =
                        prev.deref().data.get_if<decl_structs::module_template_t>()) {
                    const auto &inputs_list = module.deref().template_inputs.args;

                    auto arg_cursor = grammar::cursor_helper_t{med.value().children()};
                    for (auto i = 0; i < inputs_list.size(); i++) {
                        if (!arg_cursor.within())
                            throw std::runtime_error(
                                "Template init requires more parameters");

                        const auto &input_driver = inputs_list[i];
                        auto ambi = arg_cursor.must_extract<medianc::AMBIGUOUS>();
                        ovisit(
                            input_driver.deref().data,
                            [&](const decl_structs::type_alias_t &) {
                                std::println("TYPE");
                            },

                            [](auto &v) { std::println("{}", type_str(v)); });

                        arg_cursor.tuple_extract<medianc::any, tokc::any>();
                    }
                    if (arg_cursor.within())
                        throw std::runtime_error(
                            "Template init requires less parameters");

                } else {
                    throw std::runtime_error("Template init on a non-template symbol");
                }

                throw std::runtime_error("template init is not supported yet");
            }

            return false;
        }

        template <bool /*is_first_call*/>
        bool process_function_call(ctx_t &ctx,
                                   ref_symbols symbols,
                                   grammar::cursor_helper_t &cursor) {
            if (auto med = cursor.extract<medianc::FUNCTION_CALL>()) {
                vector<ref_expr> args;
                auto prev = result.back();
                {
                    auto cursor = grammar::cursor_helper_t{med.value().children()};
                    auto env = make<env_t>::call(ctx, symbols, caller, ctx.pool());
                    while (cursor.within()) {
                        auto expr_med = cursor.must_extract<medianc::EXPR>();
                        auto arg = node2ast::expr_fn(env, expr_med.children());

                        resolve_pass r{ctx};
                        r.entry(arg.deref().ast);

                        args.push_back(arg);
                    }
                }
                throw std::runtime_error("function calls are not supported yet");
            }
            return false;
        }

        ref_decl resolve_dealias(ctx_t &ctx, ref_decl isymbol) {
            auto symbol = dealias(isymbol);
            while (is_incomplete(symbol)) {
                handle_incomplete(ctx, symbol);
                symbol = dealias(symbol);
            }
            return symbol;
        }

        template <bool is_first_call>
        bool
        process_id(ctx_t &ctx, ref_symbols &symbols, grammar::cursor_helper_t &cursor) {
            if (auto fin = cursor.extract<tokc::ID>()) {
                auto id = ctx.toks().str(fin.value());

                auto [found, osymbol] = get_lookup_fn(is_first_call)(symbols, id);

                if (!found) [[unlikely]] {
                    std::println("Failed to resolve symbol");
                    return false;
                }
                auto symbol = resolve_dealias(ctx, osymbol.value());

                result.push_back(symbol);

                if (!cursor.within())
                    return true;

                auto new_symbols = get_symbols(symbol);
                if (!new_symbols) [[unlikely]] {
                    std::println("Failed to extract symbols");
                    return false;
                }
                symbols = new_symbols;
                return true;
            }
            return false;
        }

        template <bool is_first_call>
        void path(ctx_t &ctx, ref_symbols symbols, grammar::cursor_helper_t &cursor) {
            if (!cursor.within())
                return;

            if (process_template_init<is_first_call>(ctx, symbols, cursor) ||
                process_function_call<is_first_call>(ctx, symbols, cursor) ||
                process_id<is_first_call>(ctx, symbols, cursor)) {
                [[clang::musttail]] return path<false>(ctx, symbols, cursor);
            }

            if (result.size()) {
                for (auto &r : result)
                    std::print("{}, ", r.deref().name);
                std::putchar('\n');
            }

            throw std::runtime_error("Not supported yet");
        }

        void entry(ctx_t &ctx, ref_symbols symbols, grammar::cursor_helper_t &cursor) {
            auto p = cursor.extract<tokc::BUILTIN_DUCKLING>();
            if (p)
                symbols = symbols_t::get_root(symbols);
            return path<true>(ctx, symbols, cursor);
        }

      public:
        result_t entry(ctx_t &ctx, ref_symbols symbols, median_t med) && {
            auto cursor = grammar::cursor_helper_t{med.children()};
            entry(ctx, symbols, cursor);
            return result;
        }

        static void handle(ctx_t &ctx, ref_decl ptr, decl_structs::incomplete::alias i) {
            auto v = resolver{ptr.deref().ast}.entry(ctx, i.symbols, i.med);
            auto back = v.back();

            ptr.deref().data = decl_structs::decl_alias_t{back};
        }

        static void handle(ctx_t &ctx, ref_decl ptr, decl_structs::incomplete::bind i) {
            auto &[symbols, type, init_expr] = i;
            if (auto tv = type.deref().data.get_if<type_structs::incomplete::alias>()) {
                handle(ctx, type, tv.deref());
            }
            // make sure that the type is valid
            // make sure that we generate a callback to convert the function_template
            // to a function auto& tv = dealias(type).deref().data; if
            // (tv.has<type_structs::fntemplate_t>())
            //     throw std::runtime_error("Error");
            ptr.deref().data = decl_structs::bind_t{type, init_expr.value_or(nullptr)};
        }

        static void handle(ctx_t &ctx, ref_type ptr, type_structs::incomplete::alias i) {
            auto &[symbols, med] = i;
            auto v = resolver{ptr.deref().ast}.entry(ctx, symbols, med);
            auto back = v.back();
            auto b = dealias(back);

            if (auto v = b.deref().data.get_if<decl_structs::type_alias_t>()) {
                ptr.deref().data = type_structs::alias_t{v.deref().ref};
            } else {
                throw std::runtime_error("Did not find a type, " +
                                         ptr.deref().data.type_str());
            }
        }
        static void handle(ctx_t &ctx, ref_expr ptr, expr_structs::incomplete::chain i) {}
    };

    void visit(ref_stmt) {}
    void visit(ref_stmts) {}

    void visit(ref_decl ptr) {
        using namespace decl_structs;
        return ovisit(
            ptr.deref().data,
            [&](decl_structs::bind_t &v) {},
            [&](decl_structs::incomplete::alias &v) { resolver::handle(ctx, ptr, v); },
            [&](decl_structs::incomplete::bind &v) { resolver::handle(ctx, ptr, v); },
            [](auto &v) {});
    }

    void visit(ref_type ptr) {
        if (auto v = ptr.deref().data.get_if<type_structs::incomplete::alias>())
            return resolver::handle(ctx, ptr, v.deref());
    }

    void visit(ref_expr ptr, expr_structs::operand_t &val) {
        using namespace expr_structs;
        ovisit(
            val.data,
            [&](pipe_t &v) {
                auto e = find_pipe_lhs(ptr);
                if (!e)
                    throw std::runtime_error("Failed to find pipe match");
                auto result = e;
                v.ref = result;
            },
            [&](incomplete::chain &v) { return resolver::handle(ctx, ptr, v); },
            [&](auto &) {});
        // if (auto v = val.data.get_if<expr_structs::incomplete::chain>())
        //     return resolver::handle(ctx, ptr, v.deref());
    }

    void visit(ref_expr ptr, expr_structs::operator_t &val) {}
    void visit(ref_expr ptr, std::monostate &v) {}

    ref_type resolve_expr_type(ref_expr ptr, expr_structs::operator_t &op) {
        // todo
        return ptr.deref().type;
    }

    ref_type resolve_expr_type(ref_expr ptr, expr_structs::operand_t &op) {
        using namespace expr_structs;
        auto &expr = ptr.deref();
        return ovisit(
            op.data,
            [&]<typename T>
                requires cmp_any_v<T, int_t, sizeof_type_t, sizeof_expr_t>
            (T &) { return build::tnone::const_int(ctx.pool(), expr.ast); },
            [&](float_t &) { return build::tnone::const_float(ctx.pool(), expr.ast); },
            [&](boolean_t &) { return build::tnone::const_bool(ctx.pool(), expr.ast); },
            [&](variant_init_t &val) {
                auto [name, init_expr] = val;
                auto itype = resolve_expr_type(init_expr);
                auto p = std::array{util::field{name, itype}};
                auto type = build::tnone::variant(ctx.pool(), ptr.deref().ast, p);
                return type;
            },
            [&](initlist_t &val) {
                auto tptr = build::tnone::alloc(ctx.pool(), ptr.deref().ast);
                using s = build::tnone::tup;
                auto step = s::begin(tptr.ast);
                for (ref_expr &entry : val.data)
                    s::append(step, resolve_expr_type(entry));
                tptr.data.deref().data = step.val;
                return tptr.data;
            },
            [&](pipe_t &v) {
                assert(v.ref);
                return resolve_expr_type(v.ref);
            },
            [&](fold_t &v) { return resolve_expr_type(v.ref); },
            [&](complit_t &v) { return v.type; },
            [&](auto &) { return ptr.deref().type; });
    }

    ref_type resolve_expr_type(ref_expr ptr) {
        auto &expr = ptr.deref();
        auto &type = expr.type;

        ovisit(expr.data, [this, ptr, &type]<typename T>(T &v) {
            if constexpr (cmp_any_v<T,
                                    expr_structs::operand_t,
                                    expr_structs::operator_t>) {
                ref_type rtype = resolve_expr_type(ptr, v);
                if (!rtype.deref().data.has<type_structs::placeholder>()) {
                    type_structs::variant val = type_structs::alias_t{rtype};
                    type.deref().data = std::move(val);
                }
            } else {
                std::unreachable();
            }
        });

        return type;
    }

    // this might not work as intendeed for some time
    void visit(ref_expr ptr) {
        auto &val = ptr.deref();
        auto type = val.type;
        ovisit(val.data, [this, ptr](auto &v) { visit(ptr, v); });

        if (type.deref().data.has<type_structs::placeholder>())
            resolve_expr_type(ptr);
    }

    void visit(std::nullptr_t) {}
    void visit(std::monostate) {}

    void ast_visit(ref_ast ast) {
        auto value = ast.deref();
        ovisit(value.data, [this](auto &v) -> void { return visit(v); });

        for (auto &child : value.children)
            ast_visit(child);
    }

  public:
    void entry(ref_ast root) { ast_visit(root); }
};

struct validation_pass {
  private:
    template <typename T>
    static void surface_mutability_check(const mutability::t mut, ref_type type, T &val) {

        if (mutability::is_none(mut)) // no point in checking
            return;

        using namespace type_structs;
        if constexpr (cmp<T, indirection>::value) {
            ::visit(
                val.data,
                [](ptr_t &val) [[clang::preserve_most]] {
                    auto ptr_mut_str = std::string(type_structs::ptr_mut::str(val.mut));
                    boost::algorithm::to_lower(ptr_mut_str);
                    throw std::runtime_error("Illegal use of mutability modifier on an " +
                                             ptr_mut_str + " pointer");
                },
                [](array_t &val) [[clang::preserve_most]] {
                    throw std::runtime_error(
                        "Illegal use of mutability modifier on array type");
                },
                [](optr_t &val) {},
                [](auto &val) [[clang::preserve_most]] {
                    throw std::runtime_error("Unsupported indirection type {" +
                                             type_str(val) +
                                             "} or passed the "
                                             "wrong type to this visitor.");
                });
        } else if constexpr (cmp<T, type_structs::fntemplate_t>::value) {
            throw std::runtime_error("Function template types can't have mutability");
        } else if constexpr (cmp<T, type_structs::fntype_t>::value) {
        }

        return;
    }

    static void indirection_final_stability_check(ref_type type) {
        // std::println(
        //     "{}\nThis check causes a stack overflow if we have recursive "
        //     "types using pointers\n even though they shouldn't happen\n "
        //     "we should be able to detect them"
        //     "\n=============================================",
        //     __PRETTY_FUNCTION__);
        // @TODO:
        // detect type recursion so we do not crash here
        return;
        auto fin = deref(type);
        ovisit(fin.deref().data, []<typename T>(T &val) {
            if constexpr (cmp_v<T, type_structs::void_t> ||
                          cmp_v<T, type_structs::infered_t>)
                throw std::runtime_error("Indirection points to an incomplete type");
        });
    }

    static void bind_type_check(ref_type itype) {
        auto type = deref(dealias(itype));
        const bool rule =
            belongs_to_category<append<type_structs::abstract_category,
                                       type_structs::fntemplate_t>>(type.deref().data);
        assert(!rule);
    }

    template <typename BindT>
        requires is_in_list<BindT, decl_structs::bind_cat>::value
    static void bind_check(const BindT &val) {
        bind_type_check(val.type);
    }

    static void visit(ref_type &ptr) {
        ::visit(
            ptr.deref().data,
            [ptr](type_structs::alias_t &val) {
                auto next = val.ref;
                const auto next_mut = next.deref().mut;
                const auto ptr_mut = ptr.deref().mut;
                if (mutability::has(next_mut) && mutability::has(ptr_mut) &&
                    !mutability::equals(ptr_mut, next_mut))
                    throw std::runtime_error(
                        "different mutabilities between type aliases");
            },
            [](type_structs::rec_t &val) {
                // for (auto [k, mem_ptr] : val.symbols.deref().table) {
                for (auto &[k, mem_ptr] : val.symbols.deref().table) {
                    auto &mem = mem_ptr.deref().data.get<decl_structs::rec_member_t>();
                    bind_check(mem);
                }
            },
            [](type_structs::tup_t &val) {},
            [ptr](type_structs::indirection &val) {
                surface_mutability_check(ptr.deref().mut, ptr, val);
                indirection_final_stability_check(ptr);
            },
            [ptr](type_structs::fntemplate_t &val) {
                surface_mutability_check(ptr.deref().mut, ptr, val);
                fnsig(val.sig);
            },
            [](type_structs::fntype_t &val) {},
            [](auto &val) {});
    }

    void visit(ref_expr ptr) { using namespace expr_structs; }
    static void fnsig(fnsig_t &sig) { using namespace decl_structs; }

    static auto valid_field(const auto &v) {
        assert(!v.type.deref().data.template has<type_structs::fntemplate_t>());
        assert(!v.type.deref().data.template has<type_structs::void_t>());
    }

    void visit(ref_decl &ptr) {
        using namespace decl_structs;
        ::visit(
            ptr.deref().data,
            [](bind_t &val) { valid_field(val); },
            [](rec_member_t &val) { valid_field(val); },
            [](variant_member_t &val) { valid_field(val); },
            [](argument_t &val) { valid_field(val); },
            [](function_t &val) { fnsig(val.sig); },
            [](module_t &val) {
                for (auto &v : val.frame.symbols.deref().table) {
                    const auto &data = v.second.deref().data;
                    std::visit(
                        []<typename T>(T &val) {
                            if constexpr (is_in_list<std::remove_cvref_t<T>,
                                                     bind_cat>()) {
                                assert(mutability::equals(val.type.deref().mut,
                                                          mutability::constant()));
                            }
                        },
                        data);
                }
                for (auto v : val.frame.stmts.deref().elms)
                    assert(!v.deref().data.has<stmt_structs::expr>());
            },

            [](module_template_t &val) {
                for (const auto &v : val.frame.symbols.deref().table)
                    assert(!belongs_to_category<bind_cat>(v.second.deref().data));

                for (const auto &v : val.frame.stmts.deref().elms)
                    assert(!v.deref().data.has<stmt_structs::expr>());
            },
            [](auto &val) {});
    }

  public:
    void entry(ref_ast &ptr) {
        ::visit(
            ptr.deref().data,
            [](std::monostate &val) {},
            [](std::nullptr_t &val) {},
            [](ref_stmt &val) {},
            [](ref_stmts &val) {},
            [this](auto &val) { return this->visit(val); });
        for (auto &child : ptr.deref().children)
            entry(child);
    }
};

auto parse_grammar(env_t env, ref_stmts stmts, auto ch) {
    return node2ast::stmts(env.pass(), stmts, ch);
}

// auto resolve_ast(rctx_t& rctx) { return rctx.run_all_tasks(); }

struct ignore_mutability_pol {
    static constexpr bool ignore_mutability = true;
};

void tests() {
    auto allocator = pool_t{};
    auto root = alloc_ast(allocator, nullptr, nullptr);
    {
        auto symbols = make<symbols_t>::call(allocator, nullptr);
        {
            auto decl1 = alloc_decl(allocator, root, symbols, "var");
            auto decl2 = alloc_decl(allocator, root, symbols, "var");
            assert(decl1);
            assert(!decl2);
        }
        {
            auto decl1 =
                alloc_decl<redecl_policy::reuse>(allocator, root, symbols, "var2");
            auto decl2 =
                alloc_decl<redecl_policy::reuse>(allocator, root, symbols, "var2");
            assert(decl1 && decl2);
            assert(decl1 == decl2);
        }
    }

    {
        // auto res = build::external_resource{allocator, root};

        auto f = build::tmut::float128(allocator, root);
        auto s = build::tnone::sint(allocator, root, 128);
        auto u = build::tnone::uint(allocator, root, 128);

        auto fields =
            std::array{util::field{"success", build::tmut::float32(allocator, root)},
                       util::field{"failure", build::tmut::uint(allocator, root, 32)}};

        auto var = build::tnone::variant(allocator, root, fields);
        auto rec = build::tnone::record(allocator, root, fields);
        {
            auto [ptr, ast] = build::tnone::alloc(allocator, root);
            {
                using b = build::tnone::var;
                auto step = b::begin(allocator, root, ast);
                b::append(step, allocator, root, fields);
                ptr.deref().data = std::move(step.get());
            }
            // std::println("{}", ptr.deref().data.type_str());
        }
        assert(f && s && u && var && rec);
    }
};

std::tuple<ref_ast, ref_symbols>
entry(external_ctx &&ectx, grammar::node_t &file, pool_t &pool) {
    pool_t allocator;
    ctx_t ctx{ectx.toks, ectx.smap, pool, allocator};

    auto stmts = make<stmts_t>::call(ctx.pool(), nullptr);
    auto symbols = make<symbols_t>::call(ctx.pool(), nullptr);
    auto module = decl_structs::module_t{symbols, stmts};
    auto root = stmts.ast;

    auto [validator_time, total_time] = mesure([&] {
        auto val = file.node();
        auto ch = grammar::expect<medianc::FILE>(val).children();
        auto &rctx = ctx;

        auto [___p, parse_time] = mesure([&] {
            auto env = make<env_t>::call(rctx, module.frame.symbols, root, pool);
            return parse_grammar(env.pass(), module.frame.stmts, ch);
        });

        auto [___r, resolve_time] = mesure([&] {
            resolve_pass r{ctx};
            r.entry(root);
            return 0;
        });

        ast_printer::print(root);

        auto [___v, validate_time] = mesure([&] {
            validation_pass va;
            // va.entry(root);
            return 0;
        });
        return std::tuple(parse_time, resolve_time, validate_time);
    });

    {
        std::println("\n\nmemory: {},\ntotal time: {},\nparse/resolve/validate time: {}",
                     pool.bytes_allocated(),
                     total_time,
                     validator_time);
    }

    tests();

    return {root, symbols};
}

} // namespace semantics
