#pragma once
#include "llvm/Support/Allocator.h"

//I am pretty sure this is move only
struct llvm_allocator {
    llvm::BumpPtrAllocator alloc_impl;

    struct dealloc_obj {
        void* ptr;
        void (*destructor)(void*);
    };

    std::vector<dealloc_obj> registry;
    llvm_allocator() = default;
    llvm_allocator(const llvm_allocator&) = delete;
    llvm_allocator& operator=(const llvm_allocator&) = delete;

    llvm_allocator(llvm_allocator&& other) noexcept
        : alloc_impl(std::move(other.alloc_impl)),
          registry(std::move(other.registry)) {}

    llvm_allocator& operator=(llvm_allocator&& other) noexcept {
        if (this != &other) {
            alloc_impl = std::move(other.alloc_impl);
            registry = std::move(other.registry);
        }
        return *this;
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* alloc(Args&&... args) {

        void* mem = alloc_impl.Allocate(sizeof(T), alignof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);

        if constexpr (!std::is_trivially_destructible_v<T>) {
            registry.push_back({obj, [](void* p) {
                                    static_cast<T*>(p)->~T();
                                }});
        }
        return obj;
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* alloc_array(std::size_t n, Args&&... args) {
        if (n == 0)
            return nullptr;

        void* mem = alloc_impl.Allocate(sizeof(T) * n, alignof(T));
        T* arr = static_cast<T*>(mem);

        for (std::size_t i = 0; i < n; ++i)
            new (&arr[i]) T(std::forward<Args>(args)...);

        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (std::size_t i = 0; i < n; ++i)
                registry.push_back({&arr[i], [](void* p) {
                                        static_cast<T*>(p)->~T();
                                    }});
        }
        return arr;
    }

    template <typename T>
    void destroy(T* obj) {
        if constexpr (!std::is_trivially_destructible_v<T>)
            obj->~T();
    }

    void destroy_all() {
        for (auto it = registry.rbegin(); it != registry.rend(); ++it)
            it->destructor(it->ptr);
        registry.clear();
    }

    [[nodiscard]] std::size_t bytes_allocated() const {
        return alloc_impl.getBytesAllocated();
    }
    [[nodiscard]] std::size_t total_memory() const {
        return alloc_impl.getTotalMemory();
    }
    [[nodiscard]] std::size_t num_slabs() const {
        return alloc_impl.GetNumSlabs();
    }
};
