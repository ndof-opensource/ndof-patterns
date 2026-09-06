#include <print>
#include <memory_resource>
#include <memory>
#include <type_traits>
#include <concepts>
#include <expected>
#include <cstdint>
#include <new>

// TODO: we use std::convertible_to in constructor requires clause. Can we delete this concept?
template<typename Alloc1, typename Alloc2>
concept is_allocator_compatible = requires (Alloc2 a2) {
    Alloc1(a2);
};

// concept to represent convertible from one alloc to another. is_rebindable
// requires std::same_as<
// typename std::allocator_traits<OtherAlloc>::template rebind_alloc<typename alloc_traits::value_type>,
// A
// >

enum class CloneError : std::uint8_t {
    AllocationFailed,
    ConstructionFailed,
};

template<typename T, typename InputAlloc = std::allocator<T>>
struct ICloneable {
    friend T;
    
    // TODO: confirm with Bob this is what he wants (rebind the allocator we were given to T)
    using Alloc = typename std::allocator_traits<InputAlloc>::template rebind_alloc<T>;
    // Allocator-aware deleter using allocator_traits.
    // Templated on allocator type (A) to support rebinding between related types.
    template<typename A>
    struct AllocDeleter { // TODO: clean up deleter after changes from talk 250108
        using allocator_type = A;
        using alloc_traits = std::allocator_traits<A>;

        allocator_type alloc{};

        AllocDeleter() = default;
        explicit AllocDeleter(allocator_type a) : alloc(std::move(a)) {}

        // Enable conversion from AllocDeleter<rebind> for base/derived pointer conversions
        template<typename OtherAlloc>
        requires std::same_as<
            typename std::allocator_traits<OtherAlloc>::template rebind_alloc<typename alloc_traits::value_type>,
            A
        >
        AllocDeleter(const AllocDeleter<OtherAlloc>& other) 
            : alloc(other.alloc) {}

        // Parameter type derived from the allocator's value_type, so no cast is needed.
        // unique_ptr<X, AllocDeleter<AllocForX>> passes X* directly, which matches.
        void operator()(typename alloc_traits::value_type* p) const noexcept {
            if (!p) return;
            allocator_type a = alloc;
            alloc_traits::destroy(a, p);
            alloc_traits::deallocate(a, p, 1);
        }
    };

    // Convenient alias for the primary allocator's deleter
    using Deleter = AllocDeleter<Alloc>;

    virtual ~ICloneable() = default;

private:
    [[nodiscard]] T const& self_as_derived() const noexcept {
        return static_cast<T const&>(*this);
    }

    // Store the allocator for default cloning. [[no_unique_address]] allows
    // stateless allocators (like std::allocator) to take zero space.
    [[no_unique_address]] Alloc allocator_{};
    
    // private, default constructor
    ICloneable() = default;
    // Constructor to set the allocator. Accepts any allocator convertible to Alloc.
    // TODO: Make deduction guide and remove template on PassedAlloc
    template<typename PassedAlloc>
    requires std::convertible_to<PassedAlloc, Alloc>
    explicit ICloneable(PassedAlloc a) : allocator_(Alloc(std::move(a))) {}
    // Copy constructor: copies allocator
    ICloneable(const ICloneable&) = default;
    // Move constructor: moves allocator
    ICloneable(ICloneable&&) = default;
    // Copy and move assignment deleted
    ICloneable& operator=(const ICloneable&) = delete;
    ICloneable& operator=(ICloneable&&) = delete;

public:

    // Clone using a user-provided allocator.
    // Accepts any allocator convertible to Alloc (rebound to T).
    // Delegates to virtual do_clone() for polymorphic construction.
    // Contract: the allocator must outlive the clone.
    // T, the type of the object held, must be derived from U, the type of the object to be returned.
    template<typename U, typename PassedAlloc>
    requires std::is_base_of_v<U, T>
        && std::convertible_to<PassedAlloc, Alloc>
    [[nodiscard]] std::unique_ptr<U, AllocDeleter<typename std::allocator_traits<Alloc>::template rebind_alloc<U>>> 
    clone(this U const& self, PassedAlloc passed_alloc) {
        using alloc_traits = std::allocator_traits<Alloc>;
        using ReboundAlloc = typename alloc_traits::template rebind_alloc<U>;
        using ReboundDeleter = AllocDeleter<ReboundAlloc>;

        // Convert passed allocator to Alloc (rebound to T) for do_clone
        Alloc alloc(std::move(passed_alloc));
        
        // Delegate to virtual do_clone for polymorphic construction
        std::unique_ptr<T, Deleter> r = self.do_clone(alloc);
        
        // Transfer ownership with rebound deleter
        Deleter old_deleter = r.get_deleter();
        T* obj = r.release();
        
        // Rebind the allocator from T to U (no-op if U == T)
        ReboundAlloc rebound_alloc(old_deleter.alloc);
        
        return std::unique_ptr<U, ReboundDeleter>(
            obj,
            ReboundDeleter(rebound_alloc)
        );
    }

    // noexcept variant: wraps the throwing clone, converting exceptions
    // to CloneError. Derived classes that override do_clone (throwing)
    // automatically get this noexcept path for free.
    template<typename U, typename PassedAlloc>
    requires std::is_base_of_v<U, T>
        && std::convertible_to<PassedAlloc, Alloc>
    [[nodiscard]] auto
    clone(this U const& self, PassedAlloc passed_alloc, std::nothrow_t) noexcept
        -> std::expected<
            std::unique_ptr<U, AllocDeleter<typename std::allocator_traits<Alloc>::template rebind_alloc<U>>>,
            CloneError>
    {
        try {
            return self.clone(std::move(passed_alloc));
        } catch (const std::bad_alloc&) {
            return std::unexpected(CloneError::AllocationFailed);
        } catch (...) {
            return std::unexpected(CloneError::ConstructionFailed);
        }
    }

    // Clone using the stored allocator.
    // If U == T, no rebinding needed. Otherwise, rebinds to U.
    // T, the type of the object held, must be derived from U, the type of the object to be returned.
    template<typename U>
    requires std::is_base_of_v<U, T>
    [[nodiscard]] std::unique_ptr<U, AllocDeleter<typename std::allocator_traits<Alloc>::template rebind_alloc<U>>> 
    clone(this U const& self) {
        using alloc_traits = std::allocator_traits<Alloc>;
        using ReboundAlloc = typename alloc_traits::template rebind_alloc<U>;
        using ReboundDeleter = AllocDeleter<ReboundAlloc>;

        // Delegate to virtual do_clone using the stored allocator
        std::unique_ptr<T, Deleter> r = self.do_clone(self.allocator_);
        
        // Transfer ownership with rebound deleter
        Deleter old_deleter = r.get_deleter();
        T* obj = r.release();
        
        // Rebind the allocator from T to U (no-op if U == T)
        ReboundAlloc rebound_alloc(old_deleter.alloc);
        
        return std::unique_ptr<U, ReboundDeleter>(
            obj, 
            ReboundDeleter(rebound_alloc)
        );
    }

    // noexcept variant: wraps the throwing clone (stored-allocator version).
    template<typename U>
    requires std::is_base_of_v<U, T>
    [[nodiscard]] auto
    clone(this U const& self, std::nothrow_t) noexcept
        -> std::expected<
            std::unique_ptr<U, AllocDeleter<typename std::allocator_traits<Alloc>::template rebind_alloc<U>>>,
            CloneError>
    {
        try {
            return self.clone();
        } catch (const std::bad_alloc&) {
            return std::unexpected(CloneError::AllocationFailed);
        } catch (...) {
            return std::unexpected(CloneError::ConstructionFailed);
        }
    }

protected:
    // Throwing variant: allocates and copy-constructs via allocator_traits.
    // Override in derived classes for custom polymorphic construction.
    [[nodiscard]] virtual std::unique_ptr<T, Deleter> do_clone(Alloc alloc) const {
        using alloc_traits = std::allocator_traits<Alloc>;

        T* ptr = alloc_traits::allocate(alloc, 1);
        try {
            alloc_traits::construct(alloc, ptr, self_as_derived());
            return std::unique_ptr<T, Deleter>(ptr, Deleter(alloc));
        } catch (...) {
            alloc_traits::deallocate(alloc, ptr, 1);
            throw;
        }
    }

    // noexcept variant: delegates to the throwing do_clone, catches and
    // converts exceptions to CloneError. Derived classes may override to
    // provide a more efficient non-throwing implementation.
    [[nodiscard]] virtual auto
    do_clone(Alloc alloc, std::nothrow_t) const noexcept
        -> std::expected<std::unique_ptr<T, Deleter>, CloneError>
    {
        try {
            return do_clone(std::move(alloc));
        } catch (const std::bad_alloc&) {
            return std::unexpected(CloneError::AllocationFailed);
        } catch (...) {
            return std::unexpected(CloneError::ConstructionFailed);
        }
    }
};

template<typename T>
concept Cloneable = std::is_base_of_v<ICloneable<T>, T>;
