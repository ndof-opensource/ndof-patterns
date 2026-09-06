#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include <ndof/patterns/icloneable.hpp>

// ============================================================================
// Custom Allocators
// ============================================================================

// Accumulates allocation/deallocation counts and byte volumes.
struct AllocationStats {
    std::size_t allocations       = 0;
    std::size_t deallocations     = 0;
    std::size_t bytes_allocated   = 0;
    std::size_t bytes_deallocated = 0;
};

// Wraps std::allocator and forwards every allocate/deallocate through
// a shared AllocationStats record so tests can observe allocation behavior.
template<typename T>
class TrackingAllocator {
public:
    using value_type = T;

    AllocationStats* stats_;

    explicit TrackingAllocator(AllocationStats& stats) noexcept : stats_(&stats) {}

    template<typename U>
    TrackingAllocator(const TrackingAllocator<U>& other) noexcept : stats_(other.stats_) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        stats_->allocations++;
        stats_->bytes_allocated += n * sizeof(T);
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        stats_->deallocations++;
        stats_->bytes_deallocated += n * sizeof(T);
        std::allocator<T>{}.deallocate(p, n);
    }

    [[nodiscard]] bool operator==(const TrackingAllocator& other) const noexcept {
        return stats_ == other.stats_;
    }
};

// Monotonic bump allocator backed by a fixed stack buffer.
// Deallocation is tracked but memory is never reclaimed.
// Capacity: 4096 bytes — sufficient for dozens of small test objects.
struct MemoryPool {
    static constexpr std::size_t CAPACITY = 4096;

    alignas(alignof(std::max_align_t)) std::byte storage[CAPACITY]{};
    std::size_t offset        = 0;
    std::size_t alloc_count   = 0;
    std::size_t dealloc_count = 0;

    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment) {
        const std::size_t mask    = alignment - 1;
        const std::size_t aligned = (offset + mask) & ~mask;
        if (aligned + bytes > CAPACITY) {
            throw std::bad_alloc();
        }
        void* ptr = &storage[aligned];
        offset = aligned + bytes;
        ++alloc_count;
        return ptr;
    }

    void deallocate(void* /*ptr*/, std::size_t /*bytes*/) noexcept {
        ++dealloc_count;
    }

    void reset() noexcept {
        offset        = 0;
        alloc_count   = 0;
        dealloc_count = 0;
    }

    [[nodiscard]] bool owns(const void* ptr) const noexcept {
        const auto* p = static_cast<const std::byte*>(ptr);
        return p >= storage && p < (storage + CAPACITY);
    }
};

// Standard-compliant allocator backed by a MemoryPool.
// Stateful: holds a pointer to the pool.
template<typename T>
class PoolAllocator {
public:
    using value_type = T;

    MemoryPool* pool_;

    explicit PoolAllocator(MemoryPool& pool) noexcept : pool_(&pool) {}

    template<typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool_) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        return static_cast<T*>(pool_->allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        pool_->deallocate(p, n * sizeof(T));
    }

    [[nodiscard]] bool operator==(const PoolAllocator& other) const noexcept {
        return pool_ == other.pool_;
    }
};

// ============================================================================
// Test Types
// ============================================================================

// Simple concrete type using default std::allocator.
struct Widget : ICloneable<Widget> {
    int id;
    double value;

    Widget() : ICloneable<Widget>(), id(0), value(0.0) {}
    Widget(int i, double v) : ICloneable<Widget>(), id(i), value(v) {}
    Widget(const Widget&) = default;
    Widget(Widget&&)      = default;
};

// Equivalent layout without ICloneable, for size comparison.
struct ReferenceWidget {
    virtual ~ReferenceWidget() = default;
    int id       = 0;
    double value = 0.0;
};

// Concrete type parameterized on TrackingAllocator.
struct TrackedWidget : ICloneable<TrackedWidget, TrackingAllocator<TrackedWidget>> {
    using Base = ICloneable<TrackedWidget, TrackingAllocator<TrackedWidget>>;

    int id;
    double value;

    TrackedWidget(int i, double v, TrackingAllocator<TrackedWidget> alloc)
        : Base(alloc), id(i), value(v) {}
    TrackedWidget(const TrackedWidget&) = default;
    TrackedWidget(TrackedWidget&&)      = default;
};

// Concrete type parameterized on PoolAllocator.
struct PoolWidget : ICloneable<PoolWidget, PoolAllocator<PoolWidget>> {
    using Base = ICloneable<PoolWidget, PoolAllocator<PoolWidget>>;

    int id;
    double value;

    PoolWidget(int i, double v, PoolAllocator<PoolWidget> alloc)
        : Base(alloc), id(i), value(v) {}
    PoolWidget(const PoolWidget&) = default;
    PoolWidget(PoolWidget&&)      = default;
};

// Concrete type parameterized on std::pmr::polymorphic_allocator.
struct PmrWidget : ICloneable<PmrWidget, std::pmr::polymorphic_allocator<PmrWidget>> {
    using Base = ICloneable<PmrWidget, std::pmr::polymorphic_allocator<PmrWidget>>;

    int id;
    double value;

    PmrWidget(int i, double v, std::pmr::polymorphic_allocator<PmrWidget> alloc = {})
        : Base(alloc), id(i), value(v) {}
    PmrWidget(const PmrWidget&) = default;
    PmrWidget(PmrWidget&&)      = default;
};

// Polymorphic hierarchy — concrete base so that do_clone's default
// implementation (which copy-constructs T) can be instantiated.
struct Animal : ICloneable<Animal> {
    int legs;

    Animal() : ICloneable<Animal>(), legs(0) {}
    explicit Animal(int l) : ICloneable<Animal>(), legs(l) {}
    Animal(const Animal&) = default;
    Animal(Animal&&)      = default;
    ~Animal() override    = default;

    [[nodiscard]] virtual std::string speak() const { return "..."; }
    [[nodiscard]] virtual double weight_kg() const { return 0.0; }
};

// Derived type that overrides do_clone to allocate the correct derived type.
struct Dog : Animal {
    double weight_;

    Dog() : Animal(4), weight_(0.0) {}
    explicit Dog(double w) : Animal(4), weight_(w) {}
    Dog(const Dog&) = default;
    Dog(Dog&&)      = default;

    [[nodiscard]] std::string speak() const override { return "Woof"; }
    [[nodiscard]] double weight_kg() const override { return weight_; }

protected:
    std::unique_ptr<Animal, Deleter> do_clone(Alloc alloc) const override {
        using alloc_traits = std::allocator_traits<Alloc>;
        using DogAlloc     = typename alloc_traits::template rebind_alloc<Dog>;
        using dog_traits   = std::allocator_traits<DogAlloc>;

        DogAlloc dog_alloc(alloc);
        Dog* ptr = dog_traits::allocate(dog_alloc, 1);
        try {
            dog_traits::construct(dog_alloc, ptr, *this);
            return std::unique_ptr<Animal, Deleter>(ptr, Deleter(alloc));
        } catch (...) {
            dog_traits::deallocate(dog_alloc, ptr, 1);
            throw;
        }
    }
};

struct Cat : Animal {
    bool indoor_;

    Cat() : Animal(4), indoor_(true) {}
    explicit Cat(bool ind) : Animal(4), indoor_(ind) {}
    Cat(const Cat&) = default;
    Cat(Cat&&)      = default;

    [[nodiscard]] std::string speak() const override { return "Meow"; }
    [[nodiscard]] double weight_kg() const override { return indoor_ ? 4.5 : 5.5; }

protected:
    std::unique_ptr<Animal, Deleter> do_clone(Alloc alloc) const override {
        using alloc_traits = std::allocator_traits<Alloc>;
        using CatAlloc     = typename alloc_traits::template rebind_alloc<Cat>;
        using cat_traits   = std::allocator_traits<CatAlloc>;

        CatAlloc cat_alloc(alloc);
        Cat* ptr = cat_traits::allocate(cat_alloc, 1);
        try {
            cat_traits::construct(cat_alloc, ptr, *this);
            return std::unique_ptr<Animal, Deleter>(ptr, Deleter(alloc));
        } catch (...) {
            cat_traits::deallocate(cat_alloc, ptr, 1);
            throw;
        }
    }
};

// Tracks clone generation via copy constructor — verifies that the default
// do_clone properly invokes copy construction.
struct Gadget : ICloneable<Gadget> {
    int serial;
    int clone_generation;

    explicit Gadget(int s)
        : ICloneable<Gadget>(), serial(s), clone_generation(0) {}
    Gadget(const Gadget& other)
        : ICloneable<Gadget>(other),
          serial(other.serial),
          clone_generation(other.clone_generation + 1) {}
    Gadget(Gadget&&) = default;
};

// Does not inherit from ICloneable — used to verify concept rejection.
struct NotCloneable {
    int x = 0;
};

// ============================================================================
// AllocDeleter Tests
// ============================================================================

TEST(AllocDeleter, DefaultConstruction) {
    using Deleter = ICloneable<Widget>::Deleter;
    const Deleter d{};
    d(nullptr);
    SUCCEED();
}

TEST(AllocDeleter, ConstructionWithAllocator) {
    using Deleter = ICloneable<Widget>::Deleter;
    const std::allocator<Widget> alloc;
    const Deleter d(alloc);
    d(nullptr);
    SUCCEED();
}

TEST(AllocDeleter, NullPointerIsNoOp) {
    AllocationStats stats;
    using Alloc   = TrackingAllocator<TrackedWidget>;
    using Deleter = ICloneable<TrackedWidget, Alloc>::Deleter;

    const Alloc alloc(stats);
    const Deleter d(alloc);
    d(nullptr);

    EXPECT_EQ(stats.deallocations, 0u);
    EXPECT_EQ(stats.bytes_deallocated, 0u);
}

TEST(AllocDeleter, DestroyAndDeallocateTracked) {
    AllocationStats stats;
    using Alloc        = TrackingAllocator<TrackedWidget>;
    using alloc_traits = std::allocator_traits<Alloc>;
    using Deleter      = ICloneable<TrackedWidget, Alloc>::Deleter;

    Alloc alloc(stats);
    TrackedWidget* ptr = alloc_traits::allocate(alloc, 1);
    alloc_traits::construct(alloc, ptr, 42, 3.14, alloc);

    ASSERT_EQ(stats.allocations, 1u);
    ASSERT_EQ(stats.deallocations, 0u);

    const Deleter d(alloc);
    d(ptr);

    EXPECT_EQ(stats.deallocations, 1u);
    EXPECT_EQ(stats.bytes_deallocated, sizeof(TrackedWidget));
}

TEST(AllocDeleter, ConversionBetweenRelatedAllocatorTypes) {
    // AllocDeleter<DogAlloc> converts to AllocDeleter<AnimalAlloc>
    // because rebinding DogAlloc to Animal yields AnimalAlloc.
    using DogAlloc      = std::allocator<Dog>;
    using AnimalAlloc   = std::allocator<Animal>;
    using DogDeleter    = ICloneable<Animal>::AllocDeleter<DogAlloc>;
    using AnimalDeleter = ICloneable<Animal>::AllocDeleter<AnimalAlloc>;

    const DogDeleter dog_deleter(DogAlloc{});
    const AnimalDeleter animal_deleter(dog_deleter);
    animal_deleter(nullptr);

    SUCCEED();
}

// ============================================================================
// Default Allocator (std::allocator) Tests
// ============================================================================

TEST(ICloneable_DefaultAllocator, CloneWithStoredAllocator) {
    const Widget original(42, 3.14);
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 42);
    EXPECT_DOUBLE_EQ(cloned->value, 3.14);
}

TEST(ICloneable_DefaultAllocator, CloneWithProvidedAllocator) {
    const Widget original(99, 2.718);
    const std::allocator<Widget> alloc;
    auto cloned = original.clone(alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 99);
    EXPECT_DOUBLE_EQ(cloned->value, 2.718);
}

TEST(ICloneable_DefaultAllocator, CloneProducesDistinctObject) {
    const Widget original(7, 1.0);
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_NE(static_cast<const void*>(&original),
              static_cast<const void*>(cloned.get()));
    EXPECT_EQ(cloned->id, original.id);
    EXPECT_DOUBLE_EQ(cloned->value, original.value);
}

TEST(ICloneable_DefaultAllocator, ClonedObjectIsIndependent) {
    const Widget original(10, 5.0);
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    cloned->id    = 999;
    cloned->value = 0.0;

    EXPECT_EQ(original.id, 10);
    EXPECT_DOUBLE_EQ(original.value, 5.0);
}

TEST(ICloneable_DefaultAllocator, MultipleClones) {
    const Widget original(1, 1.0);
    constexpr std::size_t NUM_CLONES = 10;

    std::vector<decltype(original.clone())> clones;
    clones.reserve(NUM_CLONES);
    for (std::size_t i = 0; i < NUM_CLONES; ++i) {
        clones.push_back(original.clone());
        ASSERT_NE(clones.back(), nullptr);
    }

    for (std::size_t i = 0; i < NUM_CLONES; ++i) {
        EXPECT_EQ(clones[i]->id, 1);
        EXPECT_DOUBLE_EQ(clones[i]->value, 1.0);
    }
}

TEST(ICloneable_DefaultAllocator, CloneOfDefaultConstructedWidget) {
    const Widget original;
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 0);
    EXPECT_DOUBLE_EQ(cloned->value, 0.0);
}

// ============================================================================
// Tracking Allocator Tests
// ============================================================================

TEST(ICloneable_TrackingAllocator, CloneAllocatesViaTracker) {
    AllocationStats stats;
    TrackingAllocator<TrackedWidget> alloc(stats);
    const TrackedWidget original(42, 3.14, alloc);

    ASSERT_EQ(stats.allocations, 0u);

    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(stats.allocations, 1u);
    EXPECT_GE(stats.bytes_allocated, sizeof(TrackedWidget));
    EXPECT_EQ(cloned->id, 42);
    EXPECT_DOUBLE_EQ(cloned->value, 3.14);
}

TEST(ICloneable_TrackingAllocator, DestructionDeallocatesViaTracker) {
    AllocationStats stats;
    TrackingAllocator<TrackedWidget> alloc(stats);
    const TrackedWidget original(7, 2.0, alloc);

    {
        auto cloned = original.clone();
        ASSERT_NE(cloned, nullptr);
        ASSERT_EQ(stats.allocations, 1u);
        ASSERT_EQ(stats.deallocations, 0u);
    }

    EXPECT_EQ(stats.deallocations, 1u);
    EXPECT_EQ(stats.bytes_deallocated, stats.bytes_allocated);
}

TEST(ICloneable_TrackingAllocator, CloneWithDifferentTracker) {
    AllocationStats original_stats;
    AllocationStats clone_stats;
    TrackingAllocator<TrackedWidget> original_alloc(original_stats);
    TrackingAllocator<TrackedWidget> clone_alloc(clone_stats);

    const TrackedWidget original(100, 9.81, original_alloc);
    auto cloned = original.clone(clone_alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(original_stats.allocations, 0u);
    EXPECT_EQ(clone_stats.allocations, 1u);
    EXPECT_EQ(cloned->id, 100);
    EXPECT_DOUBLE_EQ(cloned->value, 9.81);
}

TEST(ICloneable_TrackingAllocator, MultipleClonesAllocateAndDeallocateCorrectly) {
    AllocationStats stats;
    TrackingAllocator<TrackedWidget> alloc(stats);
    const TrackedWidget original(1, 1.0, alloc);

    constexpr std::size_t NUM_CLONES = 5;
    {
        std::vector<decltype(original.clone())> clones;
        clones.reserve(NUM_CLONES);
        for (std::size_t i = 0; i < NUM_CLONES; ++i) {
            clones.push_back(original.clone());
            ASSERT_NE(clones.back(), nullptr);
        }
        EXPECT_EQ(stats.allocations, NUM_CLONES);
        EXPECT_EQ(stats.deallocations, 0u);
    }

    EXPECT_EQ(stats.allocations, NUM_CLONES);
    EXPECT_EQ(stats.deallocations, NUM_CLONES);
}

// ============================================================================
// Pool Allocator (pre-allocated memory) Tests
// ============================================================================

TEST(ICloneable_PoolAllocator, CloneFromPool) {
    MemoryPool pool;
    PoolAllocator<PoolWidget> alloc(pool);
    const PoolWidget original(42, 6.28, alloc);

    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_TRUE(pool.owns(cloned.get()));
    EXPECT_EQ(pool.alloc_count, 1u);
    EXPECT_EQ(cloned->id, 42);
    EXPECT_DOUBLE_EQ(cloned->value, 6.28);
}

TEST(ICloneable_PoolAllocator, CloneWithExplicitPoolAllocator) {
    MemoryPool original_pool;
    PoolAllocator<PoolWidget> original_alloc(original_pool);
    const PoolWidget original(7, 1.41, original_alloc);

    MemoryPool clone_pool;
    PoolAllocator<PoolWidget> clone_alloc(clone_pool);
    auto cloned = original.clone(clone_alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_TRUE(clone_pool.owns(cloned.get()));
    EXPECT_FALSE(original_pool.owns(cloned.get()));
    EXPECT_EQ(clone_pool.alloc_count, 1u);
    EXPECT_EQ(original_pool.alloc_count, 0u);
}

TEST(ICloneable_PoolAllocator, MultipleClonesFromSamePool) {
    MemoryPool pool;
    PoolAllocator<PoolWidget> alloc(pool);
    const PoolWidget original(1, 1.0, alloc);

    constexpr std::size_t NUM_CLONES = 8;
    std::vector<decltype(original.clone())> clones;
    clones.reserve(NUM_CLONES);
    for (std::size_t i = 0; i < NUM_CLONES; ++i) {
        clones.push_back(original.clone());
        ASSERT_NE(clones.back(), nullptr);
        EXPECT_TRUE(pool.owns(clones.back().get()));
    }

    EXPECT_EQ(pool.alloc_count, NUM_CLONES);
    const std::size_t expected_min_bytes = NUM_CLONES * sizeof(PoolWidget);
    EXPECT_GE(pool.offset, expected_min_bytes);
}

TEST(ICloneable_PoolAllocator, PoolTracksDeallocation) {
    MemoryPool pool;
    PoolAllocator<PoolWidget> alloc(pool);
    const PoolWidget original(1, 1.0, alloc);

    {
        auto cloned = original.clone();
        ASSERT_NE(cloned, nullptr);
        EXPECT_EQ(pool.dealloc_count, 0u);
    }

    EXPECT_EQ(pool.dealloc_count, 1u);
}

// ============================================================================
// PMR Allocator Tests
// ============================================================================

TEST(ICloneable_PmrAllocator, CloneWithMonotonicBufferResource) {
    alignas(alignof(std::max_align_t)) std::byte buffer[1024]{};
    std::pmr::monotonic_buffer_resource resource(
        buffer, sizeof(buffer), std::pmr::null_memory_resource());
    std::pmr::polymorphic_allocator<PmrWidget> alloc(&resource);

    const PmrWidget original(42, 2.718, alloc);
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 42);
    EXPECT_DOUBLE_EQ(cloned->value, 2.718);

    const auto* raw = reinterpret_cast<const std::byte*>(cloned.get());
    EXPECT_GE(raw, static_cast<const std::byte*>(buffer));
    EXPECT_LT(raw, static_cast<const std::byte*>(buffer) + sizeof(buffer));
}

TEST(ICloneable_PmrAllocator, CloneWithExplicitPmrAllocator) {
    alignas(alignof(std::max_align_t)) std::byte original_buf[1024]{};
    std::pmr::monotonic_buffer_resource original_res(
        original_buf, sizeof(original_buf), std::pmr::null_memory_resource());

    alignas(alignof(std::max_align_t)) std::byte clone_buf[1024]{};
    std::pmr::monotonic_buffer_resource clone_res(
        clone_buf, sizeof(clone_buf), std::pmr::null_memory_resource());

    std::pmr::polymorphic_allocator<PmrWidget> original_alloc(&original_res);
    std::pmr::polymorphic_allocator<PmrWidget> clone_alloc(&clone_res);

    const PmrWidget original(99, 1.618, original_alloc);
    auto cloned = original.clone(clone_alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 99);
    EXPECT_DOUBLE_EQ(cloned->value, 1.618);

    const auto* raw = reinterpret_cast<const std::byte*>(cloned.get());
    EXPECT_GE(raw, static_cast<const std::byte*>(clone_buf));
    EXPECT_LT(raw, static_cast<const std::byte*>(clone_buf) + sizeof(clone_buf));
}

TEST(ICloneable_PmrAllocator, CloneWithUnsynchronizedPoolResource) {
    std::pmr::unsynchronized_pool_resource pool_resource;
    std::pmr::polymorphic_allocator<PmrWidget> alloc(&pool_resource);

    const PmrWidget original(77, 0.577, alloc);
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 77);
    EXPECT_DOUBLE_EQ(cloned->value, 0.577);
}

TEST(ICloneable_PmrAllocator, MultipleClonesInMonotonicBuffer) {
    alignas(alignof(std::max_align_t)) std::byte buffer[4096]{};
    std::pmr::monotonic_buffer_resource resource(
        buffer, sizeof(buffer), std::pmr::null_memory_resource());
    std::pmr::polymorphic_allocator<PmrWidget> alloc(&resource);

    const PmrWidget original(1, 1.0, alloc);
    constexpr std::size_t NUM_CLONES = 10;

    std::vector<decltype(original.clone())> clones;
    clones.reserve(NUM_CLONES);
    for (std::size_t i = 0; i < NUM_CLONES; ++i) {
        clones.push_back(original.clone());
        ASSERT_NE(clones.back(), nullptr);
        EXPECT_EQ(clones.back()->id, 1);

        const auto* raw = reinterpret_cast<const std::byte*>(clones.back().get());
        EXPECT_GE(raw, static_cast<const std::byte*>(buffer));
        EXPECT_LT(raw, static_cast<const std::byte*>(buffer) + sizeof(buffer));
    }
}

TEST(ICloneable_PmrAllocator, CloneOfCloneUsesOriginalStoredAllocator) {
    // When cloning with an explicit allocator, the clone's stored allocator
    // is copy-constructed from the original (not replaced by the explicit
    // allocator). A subsequent clone() on the clone therefore allocates from
    // the original's resource.
    alignas(alignof(std::max_align_t)) std::byte buf_a[1024]{};
    std::pmr::monotonic_buffer_resource res_a(
        buf_a, sizeof(buf_a), std::pmr::null_memory_resource());

    alignas(alignof(std::max_align_t)) std::byte buf_b[1024]{};
    std::pmr::monotonic_buffer_resource res_b(
        buf_b, sizeof(buf_b), std::pmr::null_memory_resource());

    std::pmr::polymorphic_allocator<PmrWidget> alloc_a(&res_a);
    std::pmr::polymorphic_allocator<PmrWidget> alloc_b(&res_b);

    const PmrWidget original(1, 1.0, alloc_a);
    auto clone1 = original.clone(alloc_b);

    ASSERT_NE(clone1, nullptr);
    const auto* raw1 = reinterpret_cast<const std::byte*>(clone1.get());
    EXPECT_GE(raw1, static_cast<const std::byte*>(buf_b));
    EXPECT_LT(raw1, static_cast<const std::byte*>(buf_b) + sizeof(buf_b));

    // clone1's stored allocator was copied from original → still alloc_a.
    auto clone2 = clone1->clone();

    ASSERT_NE(clone2, nullptr);
    const auto* raw2 = reinterpret_cast<const std::byte*>(clone2.get());
    EXPECT_GE(raw2, static_cast<const std::byte*>(buf_a));
    EXPECT_LT(raw2, static_cast<const std::byte*>(buf_a) + sizeof(buf_a));
}

// ============================================================================
// Polymorphic Hierarchy Tests
// ============================================================================

TEST(ICloneable_Polymorphic, CloneDogAsAnimal) {
    const Dog original(25.0);
    const Animal& ref = original;
    auto cloned = ref.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->speak(), "Woof");
    EXPECT_DOUBLE_EQ(cloned->weight_kg(), 25.0);
    EXPECT_EQ(cloned->legs, 4);
}

TEST(ICloneable_Polymorphic, CloneCatAsAnimal) {
    const Cat original(true);
    const Animal& ref = original;
    auto cloned = ref.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->speak(), "Meow");
    EXPECT_DOUBLE_EQ(cloned->weight_kg(), 4.5);
    EXPECT_EQ(cloned->legs, 4);
}

TEST(ICloneable_Polymorphic, ClonePreservesDynamicType) {
    const Dog dog(30.0);
    const Cat cat(false);
    const Animal& dog_ref = dog;
    const Animal& cat_ref = cat;

    auto dog_clone = dog_ref.clone();
    auto cat_clone = cat_ref.clone();

    ASSERT_NE(dog_clone, nullptr);
    ASSERT_NE(cat_clone, nullptr);
    EXPECT_EQ(dog_clone->speak(), "Woof");
    EXPECT_EQ(cat_clone->speak(), "Meow");
    EXPECT_NE(dog_clone->speak(), cat_clone->speak());
}

TEST(ICloneable_Polymorphic, ClonedObjectIsDistinctFromOriginal) {
    const Dog original(15.0);
    const Animal& ref = original;
    auto cloned = ref.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_NE(static_cast<const void*>(&original),
              static_cast<const void*>(cloned.get()));
}

TEST(ICloneable_Polymorphic, CloneWithProvidedAllocator) {
    const Dog original(20.0);
    const Animal& ref = original;
    const std::allocator<Animal> alloc;
    auto cloned = ref.clone(alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->speak(), "Woof");
    EXPECT_DOUBLE_EQ(cloned->weight_kg(), 20.0);
}

TEST(ICloneable_Polymorphic, CloneBaseTypeDirectly) {
    const Animal original(6);
    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->legs, 6);
    EXPECT_EQ(cloned->speak(), "...");
}

TEST(ICloneable_Polymorphic, MultipleDerivedTypeClones) {
    const Dog dog(12.5);
    const Cat indoor_cat(true);
    const Cat outdoor_cat(false);

    const Animal& d_ref  = dog;
    const Animal& ic_ref = indoor_cat;
    const Animal& oc_ref = outdoor_cat;

    auto d_clone  = d_ref.clone();
    auto ic_clone = ic_ref.clone();
    auto oc_clone = oc_ref.clone();

    ASSERT_NE(d_clone, nullptr);
    ASSERT_NE(ic_clone, nullptr);
    ASSERT_NE(oc_clone, nullptr);

    EXPECT_DOUBLE_EQ(d_clone->weight_kg(), 12.5);
    EXPECT_DOUBLE_EQ(ic_clone->weight_kg(), 4.5);
    EXPECT_DOUBLE_EQ(oc_clone->weight_kg(), 5.5);
}

// ============================================================================
// do_clone / Copy Construction Behavior Tests
// ============================================================================

TEST(ICloneable_CopyBehavior, DefaultDoCloneInvokesCopyConstructor) {
    const Gadget original(1001);
    auto gen1 = original.clone();

    ASSERT_NE(gen1, nullptr);
    EXPECT_EQ(gen1->serial, 1001);
    EXPECT_EQ(gen1->clone_generation, 1);
}

TEST(ICloneable_CopyBehavior, ChainedCloneIncreasesGeneration) {
    const Gadget original(500);

    auto gen1 = original.clone();
    ASSERT_NE(gen1, nullptr);
    EXPECT_EQ(gen1->clone_generation, 1);

    auto gen2 = gen1->clone();
    ASSERT_NE(gen2, nullptr);
    EXPECT_EQ(gen2->clone_generation, 2);

    auto gen3 = gen2->clone();
    ASSERT_NE(gen3, nullptr);
    EXPECT_EQ(gen3->clone_generation, 3);

    EXPECT_EQ(original.clone_generation, 0);
    EXPECT_EQ(gen1->serial, 500);
    EXPECT_EQ(gen3->serial, 500);
}

TEST(ICloneable_CopyBehavior, CloneWithExplicitAllocatorInvokesCopyConstructor) {
    const Gadget original(777);
    const std::allocator<Gadget> alloc;
    auto cloned = original.clone(alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->serial, 777);
    EXPECT_EQ(cloned->clone_generation, 1);
}

TEST(ICloneable_CopyBehavior, DoCloneOverrideIsCalledPolymorphically) {
    // Dog overrides do_clone; calling clone through Animal& must dispatch
    // to Dog::do_clone and produce a Dog (not a sliced Animal).
    const Dog original(33.3);
    const Animal& ref = original;
    auto cloned = ref.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->speak(), "Woof");
    EXPECT_DOUBLE_EQ(cloned->weight_kg(), 33.3);
}

// ============================================================================
// Concept Tests
// ============================================================================

TEST(Concepts, CloneableConceptSatisfiedByWidget) {
    EXPECT_TRUE(Cloneable<Widget>);
}

TEST(Concepts, CloneableConceptSatisfiedByAnimal) {
    EXPECT_TRUE(Cloneable<Animal>);
}

TEST(Concepts, CloneableConceptSatisfiedByGadget) {
    EXPECT_TRUE(Cloneable<Gadget>);
}

TEST(Concepts, CloneableConceptNotSatisfiedByNonCloneable) {
    EXPECT_FALSE(Cloneable<NotCloneable>);
}

TEST(Concepts, AllocatorCompatibility_SameType) {
    EXPECT_TRUE((is_allocator_compatible<
        std::allocator<int>, std::allocator<int>>));
}

TEST(Concepts, AllocatorCompatibility_RebindStdAllocator) {
    EXPECT_TRUE((is_allocator_compatible<
        std::allocator<int>, std::allocator<double>>));
}

TEST(Concepts, AllocatorCompatibility_IncompatibleTypes) {
    EXPECT_FALSE((is_allocator_compatible<
        std::allocator<int>, PoolAllocator<int>>));
}

TEST(Concepts, AllocatorCompatibility_PoolAllocatorRebind) {
    EXPECT_TRUE((is_allocator_compatible<
        PoolAllocator<int>, PoolAllocator<double>>));
}

TEST(Concepts, AllocatorCompatibility_PmrSameType) {
    EXPECT_TRUE((is_allocator_compatible<
        std::pmr::polymorphic_allocator<int>,
        std::pmr::polymorphic_allocator<int>>));
}

// ============================================================================
// Compile-Time Property Tests
// ============================================================================

TEST(ICloneable_CompileTime, StatelessAllocatorNoSizeOverhead) {
    // With [[no_unique_address]], a stateless std::allocator should not
    // inflate Widget beyond what a plain virtual struct with the same
    // data members would occupy.
    EXPECT_EQ(sizeof(Widget), sizeof(ReferenceWidget));
}

TEST(ICloneable_CompileTime, AllocDeleterIsDefaultConstructible) {
    EXPECT_TRUE(std::is_default_constructible_v<ICloneable<Widget>::Deleter>);
}

TEST(ICloneable_CompileTime, AllocDeleterIsConstructibleFromAllocator) {
    using Alloc   = std::allocator<Widget>;
    using Deleter = ICloneable<Widget>::Deleter;
    EXPECT_TRUE((std::is_constructible_v<Deleter, Alloc>));
}

TEST(ICloneable_CompileTime, ICloneableHasVirtualDestructor) {
    EXPECT_TRUE(std::has_virtual_destructor_v<ICloneable<Widget>>);
}

TEST(ICloneable_CompileTime, CopyAssignmentIsDeleted) {
    EXPECT_FALSE(std::is_copy_assignable_v<ICloneable<Widget>>);
}

TEST(ICloneable_CompileTime, MoveAssignmentIsDeleted) {
    EXPECT_FALSE(std::is_move_assignable_v<ICloneable<Widget>>);
}

// ============================================================================
// Stateful Allocator State Propagation Tests
// ============================================================================

TEST(ICloneable_StatefulAllocator, StoredAllocatorIsUsedForDefaultClone) {
    AllocationStats stats;
    TrackingAllocator<TrackedWidget> alloc(stats);
    const TrackedWidget original(10, 2.5, alloc);

    auto cloned = original.clone();

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(stats.allocations, 1u);
    EXPECT_EQ(cloned->id, 10);
    EXPECT_DOUBLE_EQ(cloned->value, 2.5);
}

TEST(ICloneable_StatefulAllocator, ProvidedAllocatorOverridesStoredForAllocation) {
    AllocationStats stored_stats;
    AllocationStats provided_stats;
    TrackingAllocator<TrackedWidget> stored_alloc(stored_stats);
    TrackingAllocator<TrackedWidget> provided_alloc(provided_stats);

    const TrackedWidget original(20, 7.0, stored_alloc);
    auto cloned = original.clone(provided_alloc);

    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(stored_stats.allocations, 0u);
    EXPECT_EQ(provided_stats.allocations, 1u);
}

TEST(ICloneable_StatefulAllocator, PoolAllocatorStatePreservedAcrossClones) {
    MemoryPool pool;
    PoolAllocator<PoolWidget> alloc(pool);
    const PoolWidget original(5, 5.0, alloc);

    auto clone1 = original.clone();
    auto clone2 = original.clone();

    ASSERT_NE(clone1, nullptr);
    ASSERT_NE(clone2, nullptr);
    EXPECT_TRUE(pool.owns(clone1.get()));
    EXPECT_TRUE(pool.owns(clone2.get()));
    EXPECT_NE(static_cast<const void*>(clone1.get()),
              static_cast<const void*>(clone2.get()));
    EXPECT_EQ(pool.alloc_count, 2u);
}

// ============================================================================
// Lifetime / Edge Case Tests
// ============================================================================

TEST(ICloneable_Lifetime, CloneSurvivesOriginalDestruction) {
    decltype(std::declval<Widget>().clone()) cloned;
    {
        const Widget original(42, 9.99);
        cloned = original.clone();
    }
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->id, 42);
    EXPECT_DOUBLE_EQ(cloned->value, 9.99);
}

TEST(ICloneable_Lifetime, CloneOfCloneSurvivesBothOriginals) {
    decltype(std::declval<Widget>().clone()) second_clone;
    {
        const Widget original(10, 1.0);
        auto first_clone = original.clone();
        ASSERT_NE(first_clone, nullptr);
        second_clone = first_clone->clone();
    }
    ASSERT_NE(second_clone, nullptr);
    EXPECT_EQ(second_clone->id, 10);
    EXPECT_DOUBLE_EQ(second_clone->value, 1.0);
}

TEST(ICloneable_Lifetime, PolymorphicCloneSurvivesOriginalDestruction) {
    decltype(std::declval<Animal>().clone()) cloned;
    {
        const Dog original(18.0);
        const Animal& ref = original;
        cloned = ref.clone();
    }
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->speak(), "Woof");
    EXPECT_DOUBLE_EQ(cloned->weight_kg(), 18.0);
}

TEST(ICloneable_Lifetime, TrackedCloneDeallocatesOnScopeExit) {
    AllocationStats stats;
    TrackingAllocator<TrackedWidget> alloc(stats);

    {
        const TrackedWidget original(1, 1.0, alloc);
        auto clone1 = original.clone();
        auto clone2 = original.clone();
        auto clone3 = original.clone();

        EXPECT_EQ(stats.allocations, 3u);
        EXPECT_EQ(stats.deallocations, 0u);
    }

    EXPECT_EQ(stats.allocations, 3u);
    EXPECT_EQ(stats.deallocations, 3u);
    EXPECT_EQ(stats.bytes_allocated, stats.bytes_deallocated);
}
