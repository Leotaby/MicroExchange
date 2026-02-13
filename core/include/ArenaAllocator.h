#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <cassert>
#include <new>

namespace micro_exchange::core {

    /**
     * ArenaAllocator — Fixed-type slab allocator for Order objects.
     *
     * Pre-allocates a contiguous slab and manages via intrusive free-list.
     * malloc is ~50-200ns; this is ~5ns. The difference matters when you're
     * doing millions of allocations per second.
     *
     * Growth: when exhausted, allocate new slab of 2x size.
     * Deallocation: push back to free-list (no system call).
     *
     * NOTE: currently never returns memory to the OS. This is fine for
     * the simulation (it exits when done) but would need periodic cleanup
     * or a high-water-mark reset for a long-running production system.
     */
template <typename T>
class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t initial_capacity = 65536)
        : capacity_(initial_capacity)
    {
        grow(initial_capacity);
    }

    ~ArenaAllocator() = default;

    // Non-copyable, movable
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&&) noexcept = default;
    ArenaAllocator& operator=(ArenaAllocator&&) noexcept = default;

    /**
     * Allocate one T from the arena.
     * Returns a pointer to uninitialized memory of sizeof(T).
     * Caller must placement-new to construct.
     */
    [[nodiscard]] T* allocate() {
        if (!free_head_) [[unlikely]] {
            grow(capacity_ * 2);
        }

        FreeNode* node = free_head_;
        free_head_ = node->next;
        ++allocated_;
        return reinterpret_cast<T*>(node);
    }

    /**
     * Return a T to the arena. Caller must have called destructor first.
     */
    void deallocate(T* ptr) noexcept {
        assert(ptr != nullptr);
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        --allocated_;
    }

    /**
     * Construct a T in-place with forwarded arguments.
     */
    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        T* ptr = allocate();
        return ::new (ptr) T(std::forward<Args>(args)...);
    }

    /**
     * Destroy and return to pool.
     */
    void destroy(T* ptr) {
        ptr->~T();
        deallocate(ptr);
    }

    [[nodiscard]] size_t allocated() const noexcept { return allocated_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    static_assert(sizeof(T) >= sizeof(FreeNode),
        "Arena element must be at least pointer-sized");

    void grow(size_t count) {
        // Allocate a contiguous slab
        auto slab = std::make_unique<std::byte[]>(count * sizeof(T));
        std::byte* raw = slab.get();

        // Thread the free-list through the slab
        for (size_t i = 0; i < count; ++i) {
            auto* node = reinterpret_cast<FreeNode*>(raw + i * sizeof(T));
            node->next = (i + 1 < count)
                ? reinterpret_cast<FreeNode*>(raw + (i + 1) * sizeof(T))
                : free_head_;  // Link to existing free-list tail
        }
        free_head_ = reinterpret_cast<FreeNode*>(raw);

        capacity_ += count;
        slabs_.push_back(std::move(slab));
    }

    FreeNode*                                  free_head_  = nullptr;
    size_t                                     capacity_   = 0;
    size_t                                     allocated_  = 0;
    std::vector<std::unique_ptr<std::byte[]>>  slabs_;
};

} // namespace micro_exchange::core
