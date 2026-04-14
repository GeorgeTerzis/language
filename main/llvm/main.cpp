// clang++ -std=c++23 main.cpp "$(llvm-config --libs)"

#include "./unit.hpp"
#include <cassert>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Alignment.h>
#include <print>
#include <stdexcept>
#include <utility>
#include <variant>

namespace backend {
template <typename T>
using c = const T&;
using twine = const llvm::Twine&;

using namespace semantics;

bool is_rec(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::rec_t>();
}
bool is_tup(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::tup_t>();
}
bool is_var(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::variant_t>();
}
bool is_sint(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::sint_t>();
}
bool is_uint(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::uint_t>();
}
bool is_any_int(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::sint_t>()
           || ty.deref().data.has<type_structs::uint_t>()
           || ty.deref().data.has<type_structs::bool_t>();
}
bool is_float(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return visit(
        ty.deref().data,
        [](const type_structs::float16_t&) {
            return true;
        },
        [](const type_structs::float32_t&) {
            return true;
        },
        [](const type_structs::float64_t&) {
            return true;
        },
        [](const type_structs::float128_t&) {
            return true;
        },
        [](const auto&) {
            return false;
        });
}
bool is_array(c<type_structs::indirection> v) {
    return v.data.has<type_structs::array_t>();
}
bool is_ptr(c<type_structs::indirection> v) {
    return v.data.has<type_structs::ptr_t>();
}
bool is_optr(c<type_structs::indirection> v) {
    return v.data.has<type_structs::optr_t>();
}
bool is_indirection(ref_type ast_type) {
    auto ty = dealias(ast_type);
    return ty.deref().data.has<type_structs::indirection>();
}

namespace ir {
struct arm {
    using emit_fn = std::function<llvm::Value*(llvm::Type*, llvm::Value*)>;

    std::uint64_t tag;
    llvm::Type* alternative;

    emit_fn _emit;

    llvm::Value* emit(llvm::Value* val) const {
        return _emit(alternative, val);
    }
};
} // namespace ir

struct layout {
    llvm::TypeSize size = llvm::TypeSize::getFixed(0);
    llvm::Align alignment{1};
    llvm::Type* alignment_type = nullptr;
};

llvm::IntegerType* get_bitwidth_type(llvm::LLVMContext& C,
                                     const std::uint64_t max_value) {
    assert(max_value > 0);
    const std::uint64_t bits = std::bit_width(max_value - 1);
    return llvm::Type::getIntNTy(C, bits);
}

inline void
accumulate_layout(layout& l, c<llvm::DataLayout> dl, llvm::Type* type) {
    const auto size = dl.getTypeAllocSize(type);
    const auto alignment = dl.getABITypeAlign(type);

    l.size = std::max(l.size, size);

    if (l.alignment < alignment) {
        l.alignment = alignment;
        l.alignment_type = type;
    }
}

//
llvm::Type* lower(unit& u, ref_type type);
llvm::Value* lower(unit& u, ref_expr ptr);

//
llvm::Type* lower(unit&, ref_type, c<std::monostate>) { std::unreachable(); }
llvm::Value* lower(unit&, ref_expr, c<std::monostate>) { std::unreachable(); }

auto lower(unit& u, ref_type ptr, c<type_structs::const_bool>) {
    return llvm::Type::getInt1Ty(u.context());
}
auto lower(unit& u, ref_type ptr, c<type_structs::void_t>) {
    return llvm::Type::getVoidTy(u.context());
}
auto lower(unit& u, ref_type ptr, c<type_structs::float16_t>) {
    return llvm::Type::getHalfTy(u.context());
}
auto lower(unit& u, ref_type ptr, c<type_structs::float32_t>) {
    return llvm::Type::getFloatTy(u.context());
}
auto lower(unit& u, ref_type ptr, c<type_structs::float64_t>) {
    return llvm::Type::getDoubleTy(u.context());
}
auto lower(unit& u, ref_type ptr, c<type_structs::float128_t>) {
    return llvm::Type::getFP128Ty(u.context());
}
auto lower(unit& u, ref_type ptr, c<type_structs::bool_t> v) {
    auto size = v.size.size;
    return llvm::Type::getIntNTy(u.context(), size);
}
auto lower(unit& u, ref_type ptr, c<type_structs::uint_t> v) {
    auto size = v.size.size;
    return llvm::Type::getIntNTy(u.context(), size);
}
auto lower(unit& u, ref_type ptr, c<type_structs::sint_t> v) {
    auto size = v.size.size;
    return llvm::Type::getIntNTy(u.context(), size);
}

auto lower(unit& u, ref_type ptr, c<type_structs::variant_t> v) {
    layout l;
    for (const auto& [_, m] : v.symbols.deref().table) {
        const auto& vmem =
            m.deref().data.unsafe_get<decl_structs::variant_member_t>();
        llvm::Type* lowered = lower(u, vmem.type);
        accumulate_layout(l, u.data_layout(), lowered);
    }

    auto tag_type =
        get_bitwidth_type(u.context(), v.symbols.deref().table.size());

    auto payload_type = [&] -> llvm::Type* {
        const auto size = l.size;
        const auto alignment_type = l.alignment_type;
        const auto alloc_size =
            u.data_layout().getTypeAllocSize(alignment_type);
        const auto count = (size + alloc_size - 1) / alloc_size;
        const auto type = (count > 1)
                              ? llvm::ArrayType::get(alignment_type, count)
                              : alignment_type;
        return type;
    }();

    return llvm::StructType::get(u.context(), {payload_type, tag_type});
}
auto lower(unit& u, ref_type ptr, c<type_structs::rec_t> v) {
    std::vector<llvm::Type*> types;
    types.reserve(v.members.size());
    for (const auto& m : v.members) {
        const auto& vmem =
            m.deref().data.unsafe_get<decl_structs::rec_member_t>();
        const auto type = vmem.type;
        llvm::Type* lowered = lower(u, type);
        types.push_back(lowered);
    }
    return llvm::StructType::get(u.context(), types);
}
auto lower(unit& u, ref_type ptr, c<type_structs::tup_t> v) {
    std::vector<llvm::Type*> types;
    types.reserve(v.members.size());
    for (const auto& m : v.members) {
        llvm::Type* lowered = lower(u, m);
        types.push_back(lowered);
    }
    return llvm::StructType::get(u.context(), types);
}
auto lower(unit& u, ref_type ptr, c<type_structs::ptr_t> v) {
    lower(u, v.type);
    return llvm::PointerType::get(u.context(), 0);
}
auto lower(unit& u, ref_type ptr, c<type_structs::optr_t> v) {
    return llvm::PointerType::get(u.context(), 0);
}
auto lower(unit& u, ref_type ptr, c<type_structs::fntype_t> v) {
    std::vector<llvm::Type*> arg_low;
    for (auto arg : v.args)
        arg_low.push_back(lower(u, arg));
    auto ret_low = lower(u, v.ret);

    auto fntype = llvm::FunctionType::get(ret_low, arg_low, false);
    assert(u.cache().function_types.insert(ptr, fntype).second);
    return llvm::PointerType::get(u.context(), 0);
}
auto lower(unit& u, ref_type ptr, c<type_structs::alias_t> v) {
    return lower(u, v.ref);
}

llvm::Type* lower(unit& u, ref_type ast) {
    {
        auto t = u.cache().types.retrieve(ast);
        if (t) {
            return t.value();
        }
    }
    auto irtype =
        ovisit(ast.deref().data, [&]<typename T>(c<T> data) -> llvm::Type* {
            using namespace type_structs;
            if constexpr (cmp_any<T,
                                  type_structs::placeholder,
                                  type_structs::typeof_t,
                                  type_structs::incomplete,
                                  type_structs::infered_t,
                                  type_structs::fntemplate_t,
                                  type_structs::const_float,
                                  type_structs::const_bool,
                                  type_structs::const_int,

                                  type_structs::indirection,

                                  type_structs::template_input_t>::value) {
                std::unreachable();
            } else {
                return lower(u, ast, data);
            }
        });

    assert(u.cache().types.insert(ast, irtype).second);
    return irtype;
}

auto emit_match(unit& u, std::span<ir::arm> arms) {}

auto emit_unwrap(unit& u,
                 c<type_structs::variant_t> var,
                 const std::uint64_t index,
                 llvm::Value* v) {}

llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::sizeof_type_t> v) {
    auto irtype = lower(u, v.val);
    auto value = u.data_layout().getTypeAllocSize(irtype);
    auto size_type = u.data_layout().getIntPtrType(u.context());
    return llvm::ConstantInt::get(size_type, value);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::sizeof_expr_t> v) {
    auto irexpr = lower(u, v.val);
    auto irtype = lower(u, v.val.deref().type);
    auto value = u.data_layout().getTypeAllocSize(irtype);
    auto size_type = u.data_layout().getIntPtrType(u.context());
    return llvm::ConstantInt::get(size_type, value);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::boolean_t> v) {
    return llvm::ConstantInt::getBool(u.context(), v.val);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::int_t> v) {
    return llvm::ConstantInt::get(u.context(), v.val);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::float_t> v) {
    return llvm::ConstantFP::get(u.context(), v.val);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::null_t>) {
    return llvm::ConstantPointerNull::get(
        llvm::PointerType::get(u.context(), 0));
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::pipe_t> v) {
    return lower(u, v.ref);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::fold_t> v) {
    return lower(u, v.ref);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::operand_t> v) {
    return ovisit(v.data, [&]<typename T>(const T& val) -> llvm::Value* {
        if constexpr (cmp_any<T,
                              expr_structs::infered_t,
                              expr_structs::variant_init_t,
                              expr_structs::if_t,
                              expr_structs::pipe_t,
                              expr_structs::chain_t,
                              expr_structs::initlist_t,
                              expr_structs::complit_t,
                              expr_structs::block_t>::value) {
            std::unreachable();
        } else {
            return lower(u, ptr, val);
        }
    });
}

llvm::Value* emit_int_to_int_cast(unit& u,
                                  llvm::Value* operand,
                                  llvm::Type* irtype,
                                  ref_type src_ast_type) {
    auto src_bits = operand->getType()->getIntegerBitWidth();
    auto dst_bits = irtype->getIntegerBitWidth();

    if (src_bits < dst_bits) {
        if (is_sint(src_ast_type)) {
            return u.builder().CreateSExt(operand, irtype);
        } else {
            return u.builder().CreateZExt(operand, irtype);
        }
    } else if (src_bits > dst_bits) {
        return u.builder().CreateTrunc(operand, irtype);
    } else {
        return operand;
    }
}

llvm::Value*
emit_float_to_float_cast(unit& u, llvm::Value* operand, llvm::Type* irtype) {
    auto src_type = operand->getType();

    if (src_type->getPrimitiveSizeInBits() < irtype->getPrimitiveSizeInBits()) {
        return u.builder().CreateFPExt(operand, irtype);
    } else if (src_type->getPrimitiveSizeInBits()
               > irtype->getPrimitiveSizeInBits()) {
        return u.builder().CreateFPTrunc(operand, irtype);
    } else {
        return operand;
    }
}

llvm::Value* emit_int_to_float_cast(unit& u,
                                    llvm::Value* operand,
                                    llvm::Type* irtype,
                                    ref_type src_ast_type) {
    if (is_sint(src_ast_type)) {
        return u.builder().CreateSIToFP(operand, irtype);
    } else {
        return u.builder().CreateUIToFP(operand, irtype);
    }
}

llvm::Value* emit_float_to_int_cast(unit& u,
                                    llvm::Value* operand,
                                    llvm::Type* irtype,
                                    ref_type dst_ast_type) {
    if (is_sint(dst_ast_type)) {
        return u.builder().CreateFPToSI(operand, irtype);
    } else {
        return u.builder().CreateFPToUI(operand, irtype);
    }
}

llvm::Value*
emit_as(unit& u, ref_expr ptr, llvm::Value* operand, c<expr_structs::uop_t> v) {
    const auto& payload =
        v.payload.unsafe_get<expr_structs::uop_t::as_payload_t>();
    const auto irtype = lower(u, payload.type);

    auto src_type = operand->getType();
    auto src_ast_type = v.operand.deref().type;
    auto dst_ast_type = payload.type;

    if (src_type->isIntegerTy() && irtype->isIntegerTy()) {
        return emit_int_to_int_cast(u, operand, irtype, src_ast_type);
    } else if (src_type->isFloatingPointTy() && irtype->isFloatingPointTy()) {
        return emit_float_to_float_cast(u, operand, irtype);
    } else if (src_type->isIntegerTy() && irtype->isFloatingPointTy()) {
        return emit_int_to_float_cast(u, operand, irtype, src_ast_type);
    } else if (src_type->isFloatingPointTy() && irtype->isIntegerTy()) {
        return emit_float_to_int_cast(u, operand, irtype, dst_ast_type);
    } else {
        return u.builder().CreateBitCast(operand, irtype);
    }
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::uop_t> v) {
    auto operand = lower(u, v.operand);
    const auto meta = v.op.meta();
    switch (static_cast<expr_structs::uop_e>(meta.op)) {
        case semantics::expr_structs::uop_e::AS:
            return emit_as(u, ptr, operand, v);
        default:
            std::println("Not implemented yet");
    }
    std::unreachable();
}

std::optional<unsigned> get_record_field_index(ref_type rec_type,
                                               std::string_view field_name) {
    auto ty = dealias(rec_type);
    if (auto rec = ty.deref().data.get_if<type_structs::rec_t>()) {
        unsigned idx = 0;
        for (const auto& member : rec.deref().members) {
            if (member.deref().name == field_name)
                return idx;
            ++idx;
        }
    }
    return std::nullopt;
}
llvm::Value* emit_record_access(unit& u,
                                llvm::Value* record,
                                ref_type rec_type,
                                std::string_view field_name) {
    auto idx = get_record_field_index(rec_type, field_name);
    if (!idx) {
        throw std::runtime_error("Field '" + std::string(field_name)
                                 + "' not found");
    }

    return u.builder().CreateStructGEP(lower(u, rec_type), record, *idx);
}
llvm::Value* emit_tuple_access(unit& u,
                               llvm::Value* tuple,
                               ref_type tup_type,
                               size_t index) {
    return u.builder().CreateStructGEP(lower(u, tup_type), tuple, index);
}

llvm::Value*
emit_add(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFAdd(lhs, rhs);
    return u.builder().CreateAdd(lhs, rhs);
}

llvm::Value*
emit_sub(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFSub(lhs, rhs);
    return u.builder().CreateSub(lhs, rhs);
}

llvm::Value*
emit_mul(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFMul(lhs, rhs);
    return u.builder().CreateMul(lhs, rhs);
}

llvm::Value*
emit_div(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFDiv(lhs, rhs);
    if (is_sint(lhs_type))
        return u.builder().CreateSDiv(lhs, rhs);
    return u.builder().CreateUDiv(lhs, rhs);
}

llvm::Value*
emit_rem(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFRem(lhs, rhs);
    if (is_sint(lhs_type))
        return u.builder().CreateSRem(lhs, rhs);
    return u.builder().CreateURem(lhs, rhs);
}

llvm::Value*
emit_eq(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFCmpOEQ(lhs, rhs);
    return u.builder().CreateICmpEQ(lhs, rhs);
}

llvm::Value*
emit_ne(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFCmpONE(lhs, rhs);
    return u.builder().CreateICmpNE(lhs, rhs);
}

llvm::Value*
emit_lt(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFCmpOLT(lhs, rhs);
    if (is_sint(lhs_type))
        return u.builder().CreateICmpSLT(lhs, rhs);
    return u.builder().CreateICmpULT(lhs, rhs);
}

llvm::Value*
emit_le(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFCmpOLE(lhs, rhs);
    if (is_sint(lhs_type))
        return u.builder().CreateICmpSLE(lhs, rhs);
    return u.builder().CreateICmpULE(lhs, rhs);
}

llvm::Value*
emit_gt(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFCmpOGT(lhs, rhs);
    if (is_sint(lhs_type))
        return u.builder().CreateICmpSGT(lhs, rhs);
    return u.builder().CreateICmpUGT(lhs, rhs);
}

llvm::Value*
emit_ge(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_float(lhs_type))
        return u.builder().CreateFCmpOGE(lhs, rhs);
    if (is_sint(lhs_type))
        return u.builder().CreateICmpSGE(lhs, rhs);
    return u.builder().CreateICmpUGE(lhs, rhs);
}

// Bitwise operations
llvm::Value* emit_and(unit& u, llvm::Value* lhs, llvm::Value* rhs) {
    return u.builder().CreateAnd(lhs, rhs);
}

llvm::Value* emit_or(unit& u, llvm::Value* lhs, llvm::Value* rhs) {
    return u.builder().CreateOr(lhs, rhs);
}

llvm::Value* emit_xor(unit& u, llvm::Value* lhs, llvm::Value* rhs) {
    return u.builder().CreateXor(lhs, rhs);
}

llvm::Value* emit_shl(unit& u, llvm::Value* lhs, llvm::Value* rhs) {
    return u.builder().CreateShl(lhs, rhs);
}

llvm::Value*
emit_shr(unit& u, llvm::Value* lhs, llvm::Value* rhs, ref_type lhs_type) {
    if (is_sint(lhs_type))
        return u.builder().CreateAShr(lhs, rhs);
    return u.builder().CreateLShr(lhs, rhs);
}

llvm::Value* emit_binop(unit& u,
                        expr_structs::bop_e op,
                        llvm::Value* lhs,
                        llvm::Value* rhs,
                        ref_type lhs_type) {
    using op_e = expr_structs::bop_e;
    switch (op) {
        case op_e::PLUS:
            return emit_add(u, lhs, rhs, lhs_type);
        case op_e::MINUS:
            return emit_sub(u, lhs, rhs, lhs_type);
        case op_e::MULT:
            return emit_mul(u, lhs, rhs, lhs_type);
        case op_e::DIV:
            return emit_div(u, lhs, rhs, lhs_type);
        case op_e::MOD:
            return emit_rem(u, lhs, rhs, lhs_type);
        case op_e::EQ:
            return emit_eq(u, lhs, rhs, lhs_type);
        case op_e::NEQ:
            return emit_ne(u, lhs, rhs, lhs_type);
        case op_e::LESS:
            return emit_lt(u, lhs, rhs, lhs_type);
        case op_e::LEQ:
            return emit_le(u, lhs, rhs, lhs_type);
        case op_e::GREATER:
            return emit_gt(u, lhs, rhs, lhs_type);
        case op_e::GEQ:
            return emit_ge(u, lhs, rhs, lhs_type);
        case op_e::AND:
            return emit_and(u, lhs, rhs);
        case op_e::OR:
            return emit_or(u, lhs, rhs);
        case op_e::XOR:
            return emit_xor(u, lhs, rhs);
        case op_e::SLEFT:
            return emit_shl(u, lhs, rhs);
        case op_e::SRIGHT:
            return emit_shr(u, lhs, rhs, lhs_type);
        case op_e::ASSIGN:
            return u.builder().CreateStore(rhs, lhs);

        default:
            throw std::runtime_error("Unimplemented binary operation");
    }
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::bop_t> v) {
    auto lhs = lower(u, v.lhs);
    auto lhs_ast_type = v.lhs.deref().type;
    auto rhs = lower(u, v.rhs);
    auto meta = v.op.meta();
    auto op = static_cast<expr_structs::bop_e>(meta.op);
    return emit_binop(u, op, lhs, rhs, lhs_ast_type);
}
llvm::Value* lower(unit& u, ref_expr ptr, c<expr_structs::operator_t> v) {
    return ovisit(v.data, [&](const auto& val) {
        return lower(u, ptr, val);
    });
}
llvm::Value* lower(unit& u, ref_expr ptr) {
    return ovisit(ptr.deref().data, [&](const auto& v) {
        return lower(u, ptr, v);
    });
}

} // namespace backend

int main() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetAsmPrinter();

    auto pool = semantics::pool_t{};
    auto root = semantics::make<semantics::ast_t>::call(pool, nullptr, nullptr);
    auto res = semantics::build::external_resource{pool, root};

    auto flt = semantics::build::tmut::float128(res);
    auto hlf = semantics::build::tnone::float16(res);

    using tnone = semantics::build::tnone;
    using vb = semantics::build::tnone::var;
    auto vs = vb::begin(res, root, nullptr);
    assert(vb::append(vs, res, {"flt", flt}));
    assert(vb::append(vs, res, {"hlf", hlf}));
    auto var = tnone::fin(res, vs.get());

    semantics::validator v;
    v.entry(root);

    backend::unit u("testing");

    auto lflt = backend::lower(u, flt);
    auto lhlf = backend::lower(u, hlf);
    auto lvar = backend::lower(u, var);

    // semantics::make<semantics::expr_t>::call(pool, nullptr, semantics::expr_structs::bop_t{})

    u.verify();
    u.print();
}
