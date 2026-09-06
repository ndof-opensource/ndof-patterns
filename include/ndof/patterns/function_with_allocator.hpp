#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ndof {

// Assumes these already exist in the same namespace:
//   enum class ec
//   template<class T> constexpr const void* type_id() noexcept
//   struct bytes
//   template<class T, class Alloc, class... Args>
//   constexpr bool nothrow_constructible_with_alloc_v
//   template<class T, class Alloc, class... Args>
//   void construct_with_optional_alloc(T*, const Alloc&, Args&&...) noexcept(...)
//   template<class AllocFamily, std::size_t SboBytes, std::size_t SboAlign>
//   class aligned_storage

template <class Signature,
          class AllocFamily = std::allocator<std::byte>,
          std::size_t SboBytes = 3 * sizeof(void*),
          std::size_t SboAlign = alignof(std::max_align_t)>
class function_with_allocator;

// ============================================================================
// throwing signature: R(Args...)
// - invocation returns R / void
// - exceptions from the wrapped callable propagate
// - copy/move/emplace remain error-coded via std::expected
// ============================================================================

template <class R, class... Args, class AllocFamily, std::size_t SboBytes, std::size_t SboAlign>
class function_with_allocator<R(Args...), AllocFamily, SboBytes, SboAlign> {
public:
  using allocator_type = AllocFamily;
  using traits         = std::allocator_traits<allocator_type>;

  function_with_allocator() noexcept(std::is_nothrow_default_constructible_v<allocator_type>) = default;

  explicit function_with_allocator(const allocator_type& a) noexcept
      : storage_(a) {}

  template <class F>
  explicit function_with_allocator(F&& f, const allocator_type& a = allocator_type{}) noexcept
      : storage_(a) {
    (void)try_emplace(std::forward<F>(f));
  }

  ~function_with_allocator() noexcept { reset(); }

  allocator_type get_allocator() const noexcept { return storage_.get_allocator(); }

  bool has_value() const noexcept { return ops_ != nullptr; }
  explicit operator bool() const noexcept { return has_value(); }

  void reset() noexcept {
    if (!ops_) return;
    ops_->destroy(*this);
    ops_ = nullptr;
    tid_ = nullptr;
  }

  template <class F>
  std::expected<std::remove_cvref_t<F>*, ec> try_emplace(F&& f) noexcept {
    using U = std::remove_cvref_t<F>;

    static_assert(std::is_object_v<U>, "Callable must be an object type.");
    static_assert(std::is_invocable_r_v<R, U&, Args...>,
                  "Callable does not match the requested signature.");

    reset();

    typename storage_type::block b{};
    auto ar = storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (!nothrow_constructible_with_alloc_v<U, allocator_type, U>) {
      storage_.deallocate(b);
      return std::unexpected(ec::construction_failed);
    }

    auto* p = static_cast<U*>(b.ptr);
    construct_with_optional_alloc<U>(p, storage_.get_allocator(), std::forward<F>(f));

    obj_ = b;
    ops_ = &ops_for<U>;
    tid_ = type_id<U>();
    return p;
  }

  std::expected<void, ec> try_copy_from(const function_with_allocator& other) noexcept {
    if (!other.ops_) return std::unexpected(ec::empty);
    return other.ops_->clone_to(*this, other);
  }

  std::expected<void, ec> try_move_from(function_with_allocator&& other) noexcept {
    if (!other.ops_) return std::unexpected(ec::empty);
    return other.ops_->move_to(*this, std::move(other));
  }

  function_with_allocator(const function_with_allocator& other) noexcept
      : storage_(traits::select_on_container_copy_construction(other.get_allocator())) {
    (void)try_copy_from(other);
  }

  function_with_allocator& operator=(const function_with_allocator& other) noexcept {
    if (this == &other) return *this;
    reset();
    if constexpr (traits::propagate_on_container_copy_assignment::value) {
      storage_ = storage_type(other.get_allocator());
    }
    (void)try_copy_from(other);
    return *this;
  }

  function_with_allocator(function_with_allocator&& other) noexcept
      : storage_(std::move(other.storage_)) {
    (void)try_move_from(std::move(other));
  }

  function_with_allocator& operator=(function_with_allocator&& other) noexcept {
    if (this == &other) return *this;
    reset();
    if constexpr (traits::propagate_on_container_move_assignment::value) {
      storage_ = std::move(other.storage_);
    }
    (void)try_move_from(std::move(other));
    return *this;
  }

  R operator()(Args... args) {
    return ops_->invoke(*this, std::forward<Args>(args)...);
  }

  R operator()(Args... args) const {
    return ops_->invoke_const(*this, std::forward<Args>(args)...);
  }

private:
  using storage_type = aligned_storage<allocator_type, SboBytes, SboAlign>;

  template <class U>
  static constexpr bool has_clone_into_v =
    requires(const U& u, bytes dst) {
      { u.clone_into(dst) } -> std::same_as<std::expected<U*, ec>>;
    };

  struct ops_t {
    void (*destroy)(function_with_allocator&) noexcept;
    std::expected<void, ec> (*move_to)(function_with_allocator&, function_with_allocator&&) noexcept;
    std::expected<void, ec> (*clone_to)(function_with_allocator&, const function_with_allocator&) noexcept;
    R (*invoke)(function_with_allocator&, Args&&...);
    R (*invoke_const)(const function_with_allocator&, Args&&...);
  };

  template <class U>
  static void destroy_impl(function_with_allocator& self) noexcept {
    std::destroy_at(static_cast<U*>(self.obj_.ptr));
    self.storage_.deallocate(self.obj_);
  }

  template <class U>
  static std::expected<void, ec>
  move_to_impl(function_with_allocator& dst, function_with_allocator&& src) noexcept {
    if constexpr (!std::is_nothrow_move_constructible_v<U>) {
      return std::unexpected(ec::not_movable);
    }

    dst.reset();

    typename storage_type::block b{};
    auto ar = dst.storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (!nothrow_constructible_with_alloc_v<U, allocator_type, U&&>) {
      dst.storage_.deallocate(b);
      return std::unexpected(ec::construction_failed);
    }

    auto* dp = static_cast<U*>(b.ptr);
    auto* sp = static_cast<U*>(src.obj_.ptr);

    construct_with_optional_alloc<U>(dp, dst.storage_.get_allocator(), std::move(*sp));

    std::destroy_at(sp);
    src.storage_.deallocate(src.obj_);
    src.ops_ = nullptr;
    src.tid_ = nullptr;

    dst.obj_ = b;
    dst.ops_ = &ops_for<U>;
    dst.tid_ = type_id<U>();
    return {};
  }

  template <class U>
  static std::expected<void, ec>
  clone_to_impl(function_with_allocator& dst, const function_with_allocator& src) noexcept {
    dst.reset();

    typename storage_type::block b{};
    auto ar = dst.storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (has_clone_into_v<U>) {
      bytes out{reinterpret_cast<std::byte*>(b.ptr), b.bytes, b.align};
      auto r = static_cast<const U*>(src.obj_.ptr)->clone_into(out);
      if (!r) {
        dst.storage_.deallocate(b);
        return std::unexpected(r.error());
      }
    } else {
      if constexpr (!std::is_nothrow_copy_constructible_v<U>) {
        dst.storage_.deallocate(b);
        return std::unexpected(ec::not_copyable);
      } else {
        std::construct_at(static_cast<U*>(b.ptr), *static_cast<const U*>(src.obj_.ptr));
      }
    }

    dst.obj_ = b;
    dst.ops_ = &ops_for<U>;
    dst.tid_ = type_id<U>();
    return {};
  }

  template <class U>
  static R invoke_impl(function_with_allocator& self, Args&&... args) {
    U& fn = *static_cast<U*>(self.obj_.ptr);
    if constexpr (std::is_void_v<R>) {
      std::invoke(fn, std::forward<Args>(args)...);
    } else {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
  }

  template <class U>
  static R invoke_const_impl(const function_with_allocator& self, Args&&... args) {
    const U& fn = *static_cast<const U*>(self.obj_.ptr);
    if constexpr (std::is_void_v<R>) {
      std::invoke(fn, std::forward<Args>(args)...);
    } else {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
  }

  template <class U>
  static inline const ops_t ops_for = {
    &destroy_impl<U>,
    &move_to_impl<U>,
    &clone_to_impl<U>,
    &invoke_impl<U>,
    &invoke_const_impl<U>
  };

private:
  storage_type storage_{};
  typename storage_type::block obj_{};
  const ops_t* ops_{nullptr};
  const void* tid_{nullptr};
};

// ============================================================================
// noexcept signature: R(Args...) noexcept
// - invocation returns std::expected<..., ec>
// - wrapped callable must also be nothrow-invocable
// ============================================================================

template <class R, class... Args, class AllocFamily, std::size_t SboBytes, std::size_t SboAlign>
class function_with_allocator<R(Args...) noexcept, AllocFamily, SboBytes, SboAlign> {
public:
  using allocator_type = AllocFamily;
  using traits         = std::allocator_traits<allocator_type>;

  function_with_allocator() noexcept(std::is_nothrow_default_constructible_v<allocator_type>) = default;

  explicit function_with_allocator(const allocator_type& a) noexcept
      : storage_(a) {}

  template <class F>
  explicit function_with_allocator(F&& f, const allocator_type& a = allocator_type{}) noexcept
      : storage_(a) {
    (void)try_emplace(std::forward<F>(f));
  }

  ~function_with_allocator() noexcept { reset(); }

  allocator_type get_allocator() const noexcept { return storage_.get_allocator(); }

  bool has_value() const noexcept { return ops_ != nullptr; }
  explicit operator bool() const noexcept { return has_value(); }

  void reset() noexcept {
    if (!ops_) return;
    ops_->destroy(*this);
    ops_ = nullptr;
    tid_ = nullptr;
  }

  template <class F>
  std::expected<std::remove_cvref_t<F>*, ec> try_emplace(F&& f) noexcept {
    using U = std::remove_cvref_t<F>;

    static_assert(std::is_object_v<U>, "Callable must be an object type.");
    static_assert(std::is_nothrow_invocable_r_v<R, U&, Args...>,
                  "Callable must be nothrow-invocable for a noexcept function wrapper.");

    reset();

    typename storage_type::block b{};
    auto ar = storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (!nothrow_constructible_with_alloc_v<U, allocator_type, U>) {
      storage_.deallocate(b);
      return std::unexpected(ec::construction_failed);
    }

    auto* p = static_cast<U*>(b.ptr);
    construct_with_optional_alloc<U>(p, storage_.get_allocator(), std::forward<F>(f));

    obj_ = b;
    ops_ = &ops_for<U>;
    tid_ = type_id<U>();
    return p;
  }

  std::expected<void, ec> try_copy_from(const function_with_allocator& other) noexcept {
    if (!other.ops_) return std::unexpected(ec::empty);
    return other.ops_->clone_to(*this, other);
  }

  std::expected<void, ec> try_move_from(function_with_allocator&& other) noexcept {
    if (!other.ops_) return std::unexpected(ec::empty);
    return other.ops_->move_to(*this, std::move(other));
  }

  function_with_allocator(const function_with_allocator& other) noexcept
      : storage_(traits::select_on_container_copy_construction(other.get_allocator())) {
    (void)try_copy_from(other);
  }

  function_with_allocator& operator=(const function_with_allocator& other) noexcept {
    if (this == &other) return *this;
    reset();
    if constexpr (traits::propagate_on_container_copy_assignment::value) {
      storage_ = storage_type(other.get_allocator());
    }
    (void)try_copy_from(other);
    return *this;
  }

  function_with_allocator(function_with_allocator&& other) noexcept
      : storage_(std::move(other.storage_)) {
    (void)try_move_from(std::move(other));
  }

  function_with_allocator& operator=(function_with_allocator&& other) noexcept {
    if (this == &other) return *this;
    reset();
    if constexpr (traits::propagate_on_container_move_assignment::value) {
      storage_ = std::move(other.storage_);
    }
    (void)try_move_from(std::move(other));
    return *this;
  }

  std::expected<R, ec> try_invoke(Args... args) noexcept {
    if (!ops_) return std::unexpected(ec::empty);
    return ops_->invoke(*this, std::forward<Args>(args)...);
  }

  std::expected<R, ec> try_invoke(Args... args) const noexcept {
    if (!ops_) return std::unexpected(ec::empty);
    return ops_->invoke_const(*this, std::forward<Args>(args)...);
  }

  std::expected<R, ec> operator()(Args... args) noexcept {
    return try_invoke(std::forward<Args>(args)...);
  }

  std::expected<R, ec> operator()(Args... args) const noexcept {
    return try_invoke(std::forward<Args>(args)...);
  }

private:
  using storage_type = aligned_storage<allocator_type, SboBytes, SboAlign>;

  template <class U>
  static constexpr bool has_clone_into_v =
    requires(const U& u, bytes dst) {
      { u.clone_into(dst) } noexcept -> std::same_as<std::expected<U*, ec>>;
    };

  struct ops_t {
    void (*destroy)(function_with_allocator&) noexcept;
    std::expected<void, ec> (*move_to)(function_with_allocator&, function_with_allocator&&) noexcept;
    std::expected<void, ec> (*clone_to)(function_with_allocator&, const function_with_allocator&) noexcept;
    std::expected<R, ec> (*invoke)(function_with_allocator&, Args&&...) noexcept;
    std::expected<R, ec> (*invoke_const)(const function_with_allocator&, Args&&...) noexcept;
  };

  template <class U>
  static void destroy_impl(function_with_allocator& self) noexcept {
    std::destroy_at(static_cast<U*>(self.obj_.ptr));
    self.storage_.deallocate(self.obj_);
  }

  template <class U>
  static std::expected<void, ec>
  move_to_impl(function_with_allocator& dst, function_with_allocator&& src) noexcept {
    if constexpr (!std::is_nothrow_move_constructible_v<U>) {
      return std::unexpected(ec::not_movable);
    }

    dst.reset();

    typename storage_type::block b{};
    auto ar = dst.storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (!nothrow_constructible_with_alloc_v<U, allocator_type, U&&>) {
      dst.storage_.deallocate(b);
      return std::unexpected(ec::construction_failed);
    }

    auto* dp = static_cast<U*>(b.ptr);
    auto* sp = static_cast<U*>(src.obj_.ptr);

    construct_with_optional_alloc<U>(dp, dst.storage_.get_allocator(), std::move(*sp));

    std::destroy_at(sp);
    src.storage_.deallocate(src.obj_);
    src.ops_ = nullptr;
    src.tid_ = nullptr;

    dst.obj_ = b;
    dst.ops_ = &ops_for<U>;
    dst.tid_ = type_id<U>();
    return {};
  }

  template <class U>
  static std::expected<void, ec>
  clone_to_impl(function_with_allocator& dst, const function_with_allocator& src) noexcept {
    dst.reset();

    typename storage_type::block b{};
    auto ar = dst.storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (has_clone_into_v<U>) {
      bytes out{reinterpret_cast<std::byte*>(b.ptr), b.bytes, b.align};
      auto r = static_cast<const U*>(src.obj_.ptr)->clone_into(out);
      if (!r) {
        dst.storage_.deallocate(b);
        return std::unexpected(r.error());
      }
    } else {
      if constexpr (!std::is_nothrow_copy_constructible_v<U>) {
        dst.storage_.deallocate(b);
        return std::unexpected(ec::not_copyable);
      } else {
        std::construct_at(static_cast<U*>(b.ptr), *static_cast<const U*>(src.obj_.ptr));
      }
    }

    dst.obj_ = b;
    dst.ops_ = &ops_for<U>;
    dst.tid_ = type_id<U>();
    return {};
  }

  template <class U>
  static std::expected<R, ec>
  invoke_impl(function_with_allocator& self, Args&&... args) noexcept {
    static_assert(std::is_nothrow_invocable_r_v<R, U&, Args...>);
    U& fn = *static_cast<U*>(self.obj_.ptr);
    if constexpr (std::is_void_v<R>) {
      std::invoke(fn, std::forward<Args>(args)...);
      return {};
    } else {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
  }

  template <class U>
  static std::expected<R, ec>
  invoke_const_impl(const function_with_allocator& self, Args&&... args) noexcept {
    static_assert(std::is_nothrow_invocable_r_v<R, const U&, Args...>);
    const U& fn = *static_cast<const U*>(self.obj_.ptr);
    if constexpr (std::is_void_v<R>) {
      std::invoke(fn, std::forward<Args>(args)...);
      return {};
    } else {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
  }

  template <class U>
  static inline const ops_t ops_for = {
    &destroy_impl<U>,
    &move_to_impl<U>,
    &clone_to_impl<U>,
    &invoke_impl<U>,
    &invoke_const_impl<U>
  };

private:
  using storage_type = aligned_storage<allocator_type, SboBytes, SboAlign>;

  storage_type storage_{};
  typename storage_type::block obj_{};
  const ops_t* ops_{nullptr};
  const void* tid_{nullptr};
};

} // namespace nasa_erasure