#pragma once

#include "../libs/llvm_allocator.hpp"
#include "../src/frontend/semantics3.cpp"
#include <flat_map>
#include <format>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMapEntry.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Analysis/TypeBasedAliasAnalysis.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/TypeSize.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <memory>
#include <print>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace backend {

struct unit {
  private:
    llvm_allocator a;

    std::unique_ptr<llvm::LLVMContext> c;
    std::unique_ptr<llvm::Module> m;
    std::unique_ptr<llvm::IRBuilder<>> b;
    std::unique_ptr<llvm::TargetMachine> tm;
    llvm::StringMap<bool> features;

    template <typename KeyT, typename ValueT>
    struct cache_map : std::flat_map<KeyT, ValueT> {
        auto insert(KeyT key, ValueT val) {
            return this->try_emplace(key, val);
        }
        [[nodiscard]] std::optional<ValueT> retrieve(KeyT key) const {
            if (auto it = this->find(key); it != this->end())
                return it->second;
            return std::nullopt;
        }
    };

    template <typename ValueT>
    using cache_set = std::flat_set<ValueT>;

    struct {
        cache_map<semantics::ref_type, llvm::Type*> types;
        cache_map<semantics::ref_type, llvm::FunctionType*> function_types;
        cache_map<semantics::ref_expr, llvm::Value*> exprs;
        // cache_set<semantics::ref_type> signed_int;
    } ca;

  public:
    unit(const unit&) = delete;
    unit& operator=(const unit&) = delete;

    unit(unit&&) noexcept = default;
    unit& operator=(unit&&) noexcept = default;

    explicit unit(std::string_view module_name) {
        c = std::make_unique<llvm::LLVMContext>();
        m = std::make_unique<llvm::Module>(module_name, *c);
        b = std::make_unique<llvm::IRBuilder<>>(*c);

        {
            auto target_triple = llvm::sys::getDefaultTargetTriple();
            llvm::Triple triple(target_triple);
            triple.normalize();
            m->setTargetTriple(triple);

            std::string error;
            const llvm::Target* target =
                llvm::TargetRegistry::lookupTarget(target_triple, error);
            if (!target) {
                llvm::errs() << error << "\n";
                throw std::runtime_error("Failed to lookup LLVM target");
            }

            llvm::TargetOptions opt;
            tm = std::unique_ptr<llvm::TargetMachine>(
                target->createTargetMachine(triple,
                                            "generic",
                                            "",
                                            opt,
                                            std::nullopt));

            m->setDataLayout(tm->createDataLayout());
        }

        {
            auto cpu = llvm::sys::getHostCPUName();
            features = llvm::sys::getHostCPUFeatures();
            std::string feature_str = std::format("CPU={}\n", cpu.str());
            int i = 0;
            for (auto& f : features) {
                if (f.second)
                    feature_str += "+";
                else
                    feature_str += "-";
                feature_str += f.first().str();

                if (((i + 1) % 10) == 0)
                    feature_str += "\n";
                else
                    feature_str += ", ";

                ++i;
            }
            std::println("{}", feature_str);
        }
    }

    auto& cache() { return ca; }
    const auto& cache() const { return ca; }
    [[gnu::always_inline]] inline auto& allocator() noexcept { return a; }
    [[gnu::always_inline]] inline auto& context() const noexcept { return *c; }
    [[gnu::always_inline]] inline auto& module() const noexcept { return *m; }
    [[gnu::always_inline]] inline auto& builder() const noexcept { return *b; }
    [[gnu::always_inline]] inline auto& data_layout() const noexcept {
        return m->getDataLayout();
    }

    void verify() const {
        if (llvm::verifyModule(module(), &llvm::errs())) {
            throw std::runtime_error("Invalid LLVM module");
        }
    }
    void print() const { module().print(llvm::outs(), nullptr); }
};

} // namespace backend
