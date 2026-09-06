#ifndef NDOF_OS_CALLABLE_TRAITS_PROXY_HPP
#define NDOF_OS_CALLABLE_TRAITS_PROXY_HPP

// #include <memory>
// #include <optional>
// #include <functional>
// #include <type_traits>
// #include <expected>
// #include <utility>
// #include <stdexcept>

#include <ndof-os/callable_traits/callable_concepts.hpp>
#include <ndof-os/callable_traits/callable_traits.hpp>
#include <ndof-os/callable_traits/qualified_by.hpp>
#include <ndof-os/callable_traits/callable_type_generator.hpp>

#include <variant>
#include <utility>

// TODO: [6/16/25] use std::common_type to allow any type to be cast to a wrapper of that type.
//                 noncopyconstructible wrapper that can be used to store any type in a std::any.


// // TODO: add support for exception handling callbacks on enter/exit.
// // TODO: Define a class that defines the callback object interface requirements alternatively.
// // TODO: Check alignment requirement, to make sure it doesn't exceed the size of the SBO space.
// //       Alignment ≤ SBO buffer alignment (usually 16)


// TODO: Verify this AI code and put in a utilities header.
namespace ndof {

    // TODO: Add checks for nothrow constructible and nothrow move constructible.
    //       I.E., If T has a nothrow constructor, then CopyableUniquePtr<T> should be nothrow constructible.

    // TODO: make sure this type supports allocator.
    // TODO: will have to add the deleter and also use the constructor of std::unique_ptr that takes a deleter
    //       and should rebind the unique pointers allocator type to the allocator type passed in.
    // TODO: allocators should not move or be copied into the target instance?  
    // TODO: if the allocator pointer is different across moves, copies or assignments, then space should be allocated for the new object 
    //       using the new type's allocator and the old object should be destroyed using the old allocator.
    //       I.E., allocators stay with their respective objects and are not shared across copies or moves.
    //       This is to ensure that the allocator is always valid and can be used to deallocate the object.

    // TODO: support the ability to pass a deleter in addition to an allocator, as with (6): 
    //    https://en.cppreference.com/w/cpp/memory/shared_ptr/shared_ptr.html

    // TODO: Replace std::any with void pointer and destruction logic.
    
    // Note: This is not thread safe

    // template<typename T, typename Alloc = std::allocator<T>>
    // struct CopyableUniquePtr {
    //     std::unique_ptr<T, Alloc> ptr;

    //     CopyableUniquePtr(std::unique_ptr<T> p) : ptr(std::move(p)) {}

    //     CopyableUniquePtr(const CopyableUniquePtr& other) 
    //         : ptr(other.ptr ? std::make_unique<T>(*other.ptr) : nullptr) {}

    //     CopyableUniquePtr& operator=(const CopyableUniquePtr& other) {
    //         if (this != &other) {
    //             ptr = other.ptr ? std::make_unique<T>(*other.ptr) : nullptr;
    //         }
    //         return *this;
    //     }

    //     // Also allow move for efficiency
    //     CopyableUniquePtr(CopyableUniquePtr&&) noexcept = default;
    //     CopyableUniquePtr& operator=(CopyableUniquePtr&&) noexcept = default;
    // };
    
    // TODO: Consider permitting logger callbacks. zero cost possible?
    // TODO: Tag methods with [[nodiscard]] where appropriate.

    template<class A, class T>
    using rebind_t = typename std::allocator_traits<A>::template rebind_alloc<T>;
 
    template<class A_provided, class A_callee>
    concept AllocCompatibleFor =
        requires { typename rebind_t<A_provided,  A_callee>; }  &&
        std::constructible_from<rebind_t<A_provided, A_callee>, A_callee>;

    template<Callable F>
    struct as_function{
        using type = typename GenerateFunction<F::qualifiers, typename F::ReturnType, typename F::ArgTypes>::type;
    };

    // TODO: add "add qualifiers" and "remove qualifiers" metafunctions.

    template<Callable F>
    using as_function_t = typename as_function<F>::type;
}

// TODO: in callable_type_generator.hpp, from line 264, the types defined should be functions, not function pointers.
namespace ndof
{

    // TODO: Pray to Buddha.  Are we willing to pay the cost of a virtual function call?
    //       Or is this too much overhead?
    //       Can we use a variant instead? 

 
    // TODO: Design decision. here.  Do we want to support a polymorphic proxy that can hold any basic_proxy type?

    template<typename R, bool is_noexcept, typename... A>
    struct even_more_basic_proxy {

    };

 
    // TODO: Why not use R, A... and is_noexcept directly?
    template <typename R, typename Alloc, bool is_noexcept_v, typename... Args>
    struct basic_proxy  
    {
    private:
        // TODO: handle is_nothrow_convertible for arguments?
        // TODO: check noexcept for all methods declared here.
        using Fn         = R(Args...) noexcept(is_noexcept);
        using ArgTypes   = typename CallableTraits<Fn>::ArgTypes;
        using ReturnType = typename CallableTraits<Fn>::ReturnType;

        consteval static bool is_noexcept() { return is_noexcept_v; }
        consteval static bool is_void_return() { return std::is_void_v<ReturnType>; }

        template<typename F, typename ...A>
        consteval static bool has_operator_parens() { 
            return requires(F f, A... a) { f(a...); }; 
        }

        template <typename>
        struct Inner;
 
        // TODO: add constraints for R and A...
        template<typename R, typename... A>
        struct Inner<R(A...) noexcept(is_noexcept())> {
            virtual ~Inner() = default;
            virtual ReturnType invoke(A&&...) noexcept(is_noexcept()) = 0;

            // TODO: add clone method that takes an allocator and returns a new instance of Inner with the same type.
            // TODO: Add copy constructor and move constructor if the type supports them.
            
        };

        // TODO: Test noexcept propagation in mismatched types.

        template<auto f, typename... A>
        struct InnerCallable;

        template<auto f, typename... A>
            requires StandaloneFunction<decltype(f)>
        struct InnerCallable<f,A...> : Inner<f> {
            ReturnType invoke(A&&... a) noexcept(is_noexcept()) override {
                return std::invoke(f, std::forward<A>(a)...);
            }
        }
  
        // TODO: Member function reference?
        // TODO: Prototype this in godbolt. 
        //.       Concerned about the type deduction of T in the constructor.
        // TODO: A functor will have to be unique.
        template<auto f, typename... A>
            requires MemberFunctionPtr<decltype(f)> 
        struct InnerCallable<f,A...> : Inner<as_function_t<decltype(f)>> {
            using F = decltype(f);
            using T = typename CallableTraits<F>::ClassType;

            using InnerAlloc = rebind_t<Alloc, InnerCallable<f,A...>>;
            InnerAlloc alloc;
            T function_obj;

            // TODO: deduction guide.
            // TODO: prototype this.  can T be deduced correctly?
            template<typename U>
                requires std::is_convertible_v<U, T>
            InnerCallable(const T& obj, const Alloc& alloc) 
                : inner_alloc{alloc}, function_obj(std::make_obj_using_allocator<T>(inner_alloc, obj)) {
            }

            ReturnType invoke(A&&... a) noexcept(is_noexcept()) override {
                return std::invoke(f, function_obj, std::forward<A>(a)...);
            }
        };

        mutable Alloc alloc;
        Inner<Fn>* inner;
 

        template <typename A>
        using InnerAlloc = rebind_t<Alloc, InnerCallable<A, ArgTypes...>>

        void destroy() noexcept(is_noexcept()) {
            // TODO: Consider a wrapper to decorate this logic, specifically, 
            //       the try/catch/terminate pattern. Pass the fn to be wrapped.
            if (inner) {
                auto cleanup_inner = []() noexcept(is_noexcept()) {
                    std::allocator_traits<Alloc>::destroy(alloc, inner);
                    std::allocator_traits<Alloc>::deallocate(alloc,inner, 1);
                };

                if constexpr (is_noexcept()) {
                    try {
                        cleanup_inner();
                    } catch (...) { 
                        // TODO: Deep meditation. How to recover or log?  User callback?
                        std::terminate();
                    }
                }
                else {
                    cleanup_inner();
                }
            }
        }

    public:
        // TODO: Fix up noexcept and trap any outbound exceptions.
        ~basic_proxy(){
            destroy();
        }
        
        // TODO: Handle exceptions and properly attribute as noexcept as necessary.
        template<auto f, AllocCompatibleFor<Alloc> A>
        basic_proxy(StandaloneFunction auto f, const A alloc = std::allocator<Fn>{}) noexcept(is_noexcept()){

        }
         
        // TODO: Call the InnerCallable<f ,ArgTypes...> constructor by taking the compile time mfp.
        template<
            typename T, 
            // TODO: Prototype this.  See if &T::operator() can be deduced correctly.
            auto mfp = &T::operator(),
            AllocCompatibleFor<Alloc> A = std::allocator<Fn>>

 
        // TODO: Make exception safe, or at least consistent.
        basic_proxy(T&& t, A alloc = A{}) noexcept(is_noexcept()) 
            : alloc(alloc)  {
                // Do nothing.
        }

        // TODO: is CallableTraits<decltype(f)> correct here?  Does the reference need to be removed?
        template<AllocCompatibleFor<Alloc> A>
        basic_proxy(Functor auto&& f, const A alloc = A{}) noexcept(is_noexcept())
            : basic_proxy<typename CallableTraits<decltype(f)>::ClassType ,decltype(f)::operator()), A>(
                std::forward<decltype(f)>(f), alloc) {
            // Do nothing.
        }

        // TODO: Add constraints to copy constructor, move constructor, copy assignment and move assignment 
        //.      exist for the Inner type.
        // TODO: Make sure those methods are defined for Inner iff the type supports them.
        //       Otherwise, the methods should be deleted.
        
        // Copy constructor
        // TODO: overkill. just take the Alloc.
        template<AllocCompatibleFor<Alloc> A>
        basic_proxy(const basic_proxy<Fn, A>& other)
            : alloc(std::allocator_traits<Alloc>::select_on_container_copy_construction(other.get_allocator())), inner(nullptr)
        {
            if (other.get()) {
                // Assume Inner has a virtual clone method that takes an allocator.
                inner = other.get()->clone(alloc);
            }
        }
 
        // TODO: Look into move_with_noexcept from C++23.
        // https://www.foonathan.net/2015/10/allocatorawarecontainer-propagation-pitfalls/
        // Move constructor.
        template<AllocCompatibleFor<Alloc> A>
        basic_proxy(basic_proxy<Fn,A>&& other) {
            // TODO: Fix me. move assignment is wrong.
            // TODO: Add better exception safety in accordance with the noexcept specification.
            if constexpr (std::allocator_traits<Alloc>::propagate_on_container_move_assignment::value) {
                alloc = std::move(other.alloc);
            }
            inner = other.inner;
            other.inner = nullptr;
        }

        // TODO: propagate_on_container_swap
        // Note: no swap() function invalidates any references, pointers, or iterators referring to the elements of the containers being swapped.
        // TODO: consider just deleting swap. Yikes!
        // Note: Swapping two container with unequal allocators who are not propagated is undefined behavior.
        template<AllocCompatibleFor<Alloc> OtherAlloc>
        void swap(basic_proxy<Fn, OtherAlloc>& other) noexcept(is_noexcept()) {
            //   Only swap allocators if they are of the same type; otherwise, the inner
            //      must be deallocated and reallocated using the new allocator.
            //      Same with the other's inner.
            //      But before they are destroyed, they must be cloned using the new allocator,
            //      but only if the allocator types are compatible. 

            // TODO: check for the same object.
            //.      use std::addressof?
            if (this == &other) {
                return;
            }
            if constexpr (std::is_same_v<Alloc, OtherAlloc>) {
                std::swap(inner, other.inner);
                std::swap(alloc, other.alloc);
            } else {
                // Clone other's inner into this using this allocator
                Inner<Fn>* new_inner = nullptr;
                if (other.inner) {
                    new_inner = other.inner->clone(alloc);
                }
                // Clone this inner into other using other's allocator
                Inner<Fn>* other_new_inner = nullptr;
                if (inner) {
                    other_new_inner = inner->clone(other.alloc);
                }
                destroy();
                other.destroy();
                inner = new_inner;
                other.inner = other_new_inner;

                // TODO: Swap allocators if they are of different types.
            }
        }

        bool has_value() const noexcept {
            return inner != nullptr;
        }
        using pointer = Inner<Fn>*;
        using allocator_type = Alloc;

        pointer get() const noexcept {
            return inner;
        }

        explicit operator bool() const noexcept {
            return has_value();
        }

        ReturnType operator()(ArgTypes... args) noexcept(is_noexcept()) {
            if (!inner) {
                if constexpr (is_noexcept()) {
                    std::terminate();
                } else {
                    throw std::bad_function_call();
                }
            }
            return inner->invoke(std::forward<ArgTypes>(args)...);
        }

        template<AllocCompatibleFor<Alloc> OtherAlloc>
        basic_proxy& operator=(const basic_proxy<Fn, OtherAlloc>& other) {
            if (reinterpret_cast<const void*>(this) != reinterpret_cast<const void*>(&other)) {
                destroy();
                alloc = std::allocator_traits<Alloc>::select_on_container_copy_construction(other.get_allocator());
                if (other.get()) {
                    // Assume Inner has a virtual clone method that takes an allocator.
                    inner = other.get()->clone(alloc);
                } else {
                    inner = nullptr;
                }
            }
            return *this;
        }

        // TODO: Investigate if the user should be able to pass a different "Fn" as long as it is convertible to the current Fn.
        //       This would be similar to std::function's assignment operator.
        //       But this would require destroying the current inner and creating a new inner of the new
        template<AllocCompatibleFor<Alloc> OtherAlloc>
        basic_proxy& operator=(basic_proxy<Fn, OtherAlloc>&& other) noexcept(
            std::allocator_traits<Alloc>::propagate_on_container_move_assignment::value || std::is_nothrow_move_assignable_v<Alloc>
        ) {
            if (reinterpret_cast<const void*>(this) != reinterpret_cast<const void*>(&other)) {
            destroy();
            if constexpr (std::allocator_traits<Alloc>::propagate_on_container_move_assignment::value) {
                alloc = std::move(other.alloc);
            }
            inner = other.inner;
            other.inner = nullptr;
            }
            return *this;
        }

        Alloc get_allocator() const{
            return alloc; 
        }
    };

    template <Function Fn>
    using Proxy = basic_proxy<Fn, std::allocator<Fn>>;

    namespace pmr{
        template <Function Fn>
        using Proxy = basic_proxy<Fn, std::pmr::polymorphic_allocator<Fn>>;
    }
}