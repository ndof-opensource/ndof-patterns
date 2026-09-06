#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <functional>
#include <type_traits>
#include <expected>
#include <utility>
#include <stdexcept>
#include "../../callable_traits/include/callable_concepts.hpp"
#include "../../callable_traits/include/callable_traits.hpp"

namespace ndof {

struct bad_proxy_call : std::bad_function_call {
    explicit bad_proxy_call(std::string msg) : _msg(std::move(msg)) {}
    const char* what() const noexcept override { return _msg.c_str(); }
private:
    std::string _msg;
};

// Concept to combine all non-Member Function Pointer types
// TODO: Should we add this to callable_concepts??
template<typename F>
concept NonMemberFunctionType = 
    Function<F> 
    || FunctionRef<F>
    || FunctionPtr<F> 
    || Functor<F>
    || StdFunction<F>;
    
// base Proxy template
template<typename F>
class Proxy;

// All simple Function-like objects
// Function | FunctionPtr | Functor | StdFunction
template <typename F>
requires NonMemberFunctionType<F>
class Proxy<F> {

    F f;
public:
    Proxy() = default;

    template<typename G>
    explicit Proxy(G&& f_)
    requires std::constructible_from<F, G> : f(std::forward<G>(f_)) {}

    bool is_valid() const {
        if constexpr (FunctionPtr<F>) {
            return f != nullptr;
        } else if constexpr (StdFunction<F>) {
            return static_cast<bool>(f);
        } else {
            return true;
        }
    }
    template<typename... Args>
    auto operator()(Args&&... args) const noexcept
        -> std::expected<std::invoke_result_t<F, Args...>, std::exception_ptr>
    {
        if (!is_valid()) {
            return std::unexpected(std::make_exception_ptr(bad_proxy_call{"Proxy: callable target is expired or uninitialized"}));
        }

        try {
            return std::invoke(f, std::forward<Args>(args)...);
        } catch (...) {
            return std::unexpected(std::current_exception());
        }
    }
};

// Weak Pointer
template<typename F>
requires NonMemberFunctionType<F>
class Proxy<std::weak_ptr<F>> {
    std::weak_ptr<F> f;

public:
    Proxy() = default;

    template<typename G>
    requires std::is_convertible_v<std::weak_ptr<G>, std::weak_ptr<F>>
    explicit Proxy(std::weak_ptr<G> f_)
        : f(std::move(f_)) {}

    bool is_valid() const {
        return !f.expired(); // still useful for light checks
    }

    template<typename... Args>
    auto operator()(Args&&... args) const noexcept
        -> std::expected<std::invoke_result_t<F&, Args...>, std::exception_ptr>
    {
        auto sp = f.lock();
        if (!sp) {
            return std::unexpected(std::make_exception_ptr(bad_proxy_call{"Proxy: callable target is expired or uninitialized"}));
        }

        try {
            return std::invoke(*sp, std::forward<Args>(args)...);
        } catch (...) {
            return std::unexpected(std::current_exception());
        }
    }
};

// Member function pointer
template<MemberFunctionPtr F>
class Proxy<F> {
    // Use CallableTraits to extract object type
    using Traits = CallableTraits<F>;
    using ObjectType = typename Traits::ClassType;

private:
    F member_ptr{};
    ObjectType* object_ptr{};

public:
    Proxy() = default;

    template<typename Obj>
    requires std::is_convertible_v<Obj*, ObjectType*>
    Proxy(F fn, Obj* obj) : member_ptr(fn), object_ptr(obj) {}

    bool is_valid() const {
        return object_ptr != nullptr;
    }
    template <typename... Args>
    auto operator()(Args&&... args) const noexcept
        -> std::expected<std::invoke_result_t<F, ObjectType*, Args...>, std::exception_ptr>
    {
        if (object_ptr) {
            try {
                return std::invoke(member_ptr, object_ptr, std::forward<Args>(args)...);
            } catch (...) {
                return std::unexpected(std::current_exception());
            }
        } 
        // object_ptr was invalid
        return std::unexpected(std::make_exception_ptr(
            bad_proxy_call{"Member function proxy object is null"}
        ));
    }

};

// helper traits for make_proxy handling of lambdas
template<typename T> struct is_shared_ptr                       : std::false_type {};
template<typename U> struct is_shared_ptr<std::shared_ptr<U>>   : std::true_type {};

template<typename T> struct is_weak_ptr                         : std::false_type {};
template<typename U> struct is_weak_ptr<std::weak_ptr<U>>       : std::true_type {};

// Helper function to correctly build a Proxy regardless of what user passes

// shared_ptr  →  Proxy<weak_ptr<F>>
template<typename F>
auto make_proxy(const std::shared_ptr<F>& sp)
{
    return Proxy<std::weak_ptr<F>>(std::weak_ptr<F>(sp));
}

// weak_ptr  →  Proxy<weak_ptr<F>>      (for symmetry / zero-copy)
template<typename F>
auto make_proxy(const std::weak_ptr<F>& wp)
{
    return Proxy<std::weak_ptr<F>>(wp);
}

// member-function pointer + object pointer  →  Proxy<MF>
template<MemberFunctionPtr MF, typename Obj>
auto make_proxy(MF mf, Obj* obj)
    -> Proxy<MF>
{
    static_assert(std::is_convertible_v<Obj*, typename CallableTraits<MF>::ClassType*>,
                  "Object pointer is not compatible with the member-function’s class type");
    return Proxy<MF>(mf, obj);
}

// member-function pointer + object reference  →  Proxy<MF>
template<MemberFunctionPtr MF, typename Obj>
auto make_proxy(MF mf, Obj& obj)
    -> Proxy<MF>
{
    static_assert(std::is_convertible_v<Obj*, typename CallableTraits<MF>::ClassType*>,
                  "Object reference is not compatible with the member-function’s class type");
    return Proxy<MF>(mf, &obj);
}

// fallback  (anything else)  →  Proxy<decayed F>
template<typename F>
requires (!MemberFunctionPtr<std::decay_t<F>> &&
          !is_shared_ptr<std::decay_t<F>>::value &&
          !is_weak_ptr<std::decay_t<F>>::value)
auto make_proxy(F&& f)
{
    return Proxy<std::decay_t<F>>(std::forward<F>(f));
}

} // namespace ndof


