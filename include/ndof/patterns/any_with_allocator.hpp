 #pragma once

#include <expected>
#include <memory>
#include <type_traits>
#include <utility>
#include <cstring>

namespace ndof {

// Assumes these already exist in the namespace:
//   enum class ec
//   struct bytes
//   template<class T> constexpr const void* type_id() noexcept
//   template<class T, class Alloc, class... Args>
//   constexpr bool nothrow_constructible_with_alloc_v
//   template<class T, class Alloc, class... Args>
//   void construct_with_optional_alloc(T*, const Alloc&, Args&&...) noexcept(...)
//   template<class AllocFamily, std::size_t SboBytes, std::size_t SboAlign>
//   class aligned_storage

template <class AllocFamily = std::allocator<std::byte>,
          std::size_t SboBytes = 3 * sizeof(void*),
          std::size_t SboAlign = alignof(std::max_align_t)>
class any_with_allocator {
public:
  using allocator_type = AllocFamily;
  using traits         = std::allocator_traits<allocator_type>;

  any_with_allocator() noexcept(std::is_nothrow_default_constructible_v<allocator_type>) = default;

  explicit any_with_allocator(const allocator_type& a) noexcept
      : storage_(a) {}

  ~any_with_allocator() noexcept { reset(); }

  allocator_type get_allocator() const noexcept { return storage_.get_allocator(); }

  bool has_value() const noexcept { return ops_ != nullptr; }
  const void* held_type_id() const noexcept { return tid_; }

  void reset() noexcept {
    if (!ops_) return;
    ops_->destroy(*this);
    ops_ = nullptr;
    tid_ = nullptr;
  }

  // --------------------------------------------------------------------------
  // Core API: always std::expected
  // --------------------------------------------------------------------------

  template <class T, class... Args>
  std::expected<T*, ec> try_emplace(Args&&... args) noexcept {
    using U = std::remove_cvref_t<T>;
    reset();

    typename storage_type::block b{};
    auto ar = storage_.allocate(b, sizeof(U), alignof(U));
    if (!ar) return std::unexpected(ar.error());

    if constexpr (!nothrow_constructible_with_alloc_v<U, allocator_type, Args...>) {
      storage_.deallocate(b);
      return std::unexpected(ec::construction_failed);
    }

    auto* p = static_cast<U*>(b.ptr);
    construct_with_optional_alloc<U>(p, storage_.get_allocator(), std::forward<Args>(args)...);

    obj_ = b;
    ops_ = &ops_for<U>;
    tid_ = type_id<U>();
    return p;
  }

  std::expected<void, ec> try_copy_from(const any_with_allocator& other) noexcept {
    if (!other.ops_) return std::unexpected(ec::empty);
    return other.ops_->clone_to(*this, other);
  }

  std::expected<void, ec> try_move_from(any_with_allocator&& other) noexcept {
    if (!other.ops_) return std::unexpected(ec::empty);
    return other.ops_->move_to(*this, std::move(other));
  }

  template <class T>
  std::expected<T*, ec> try_get_if() noexcept {
    using U = std::remove_cvref_t<T>;
    if (!ops_) return std::unexpected(ec::empty);
    if (tid_ != type_id<U>()) return std::unexpected(ec::type_mismatch);
    return static_cast<U*>(obj_.ptr);
  }

  template <class T>
  std::expected<const T*, ec> try_get_if() const noexcept {
    using U = std::remove_cvref_t<T>;
    if (!ops_) return std::unexpected(ec::empty);
    if (tid_ != type_id<U>()) return std::unexpected(ec::type_mismatch);
    return static_cast<const U*>(obj_.ptr);
  }

  // --------------------------------------------------------------------------
  // Convenience API: stable shape, no std::expected here
  // --------------------------------------------------------------------------

  template <class T, class... Args>
  T* emplace_ptr(Args&&... args) noexcept {
    auto r = try_emplace<T>(std::forward<Args>(args)...);
    return r ? *r : nullptr;
  }

  template <class T>
  T* get_if() noexcept {
    auto r = try_get_if<T>();
    return r ? *r : nullptr;
  }

  template <class T>
  const T* get_if() const noexcept {
    auto r = try_get_if<T>();
    return r ? *r : nullptr;
  }

  // --------------------------------------------------------------------------
  // Value semantics
  // --------------------------------------------------------------------------

  any_with_allocator(const any_with_allocator& other) noexcept
      : storage_(traits::select_on_container_copy_construction(other.get_allocator())) {
    (void)try_copy_from(other);
  }

  any_with_allocator& operator=(const any_with_allocator& other) noexcept {
    if (this == &other) return *this;
    reset();
    if constexpr (traits::propagate_on_container_copy_assignment::value) {
      storage_ = storage_type(other.get_allocator());
    }
    (void)try_copy_from(other);
    return *this;
  }

  any_with_allocator(any_with_allocator&& other) noexcept
      : storage_(std::move(other.storage_)) {
    (void)try_move_from(std::move(other));
  }

  any_with_allocator& operator=(any_with_allocator&& other) noexcept {
    if (this == &other) return *this;
    reset();
    if constexpr (traits::propagate_on_container_move_assignment::value) {
      storage_ = std::move(other.storage_);
    }
    (void)try_move_from(std::move(other));
    return *this;
  }

private:
  using storage_type = aligned_storage<allocator_type, SboBytes, SboAlign>;

  template <class U>
  static constexpr bool has_clone_into_v =
    requires(const U& u, bytes dst) {
      { u.clone_into(dst) } noexcept -> std::same_as<std::expected<U*, ec>>;
    };

  struct ops_t {
    void (*destroy)(any_with_allocator&) noexcept;
    std::expected<void, ec> (*move_to)(any_with_allocator&, any_with_allocator&&) noexcept;
    std::expected<void, ec> (*clone_to)(any_with_allocator&, const any_with_allocator&) noexcept;
  };

  template <class U>
  static void destroy_impl(any_with_allocator& self) noexcept {
    std::destroy_at(static_cast<U*>(self.obj_.ptr));
    self.storage_.deallocate(self.obj_);
  }

  // safer move: always deep-move into destination allocator domain
  template <class U>
  static std::expected<void, ec>
  move_to_impl(any_with_allocator& dst, any_with_allocator&& src) noexcept {
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
  clone_to_impl(any_with_allocator& dst, const any_with_allocator& src) noexcept {
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
  static inline const ops_t ops_for = {
    &destroy_impl<U>,
    &move_to_impl<U>,
    &clone_to_impl<U>
  };

private:
  storage_type storage_{};
  typename storage_type::block obj_{};
  const ops_t* ops_{nullptr};
  const void* tid_{nullptr};
};

} // namespace nasa_erasure