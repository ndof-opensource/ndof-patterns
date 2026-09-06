// TODO: consider std::nothrow_t constructions.

template <class AllocFamily,
          std::size_t SboBytes,
          std::size_t SboAlign>
class aligned_storage {
public:
  using allocator_type = AllocFamily;
  using traits         = std::allocator_traits<allocator_type>;

  aligned_storage() = default;
  explicit aligned_storage(const allocator_type& a) noexcept : alloc_(a) {}

  allocator_type get_allocator() const noexcept { return alloc_; }

  // TODO: Share chat session w/ peter.
  struct block {
    void* ptr{};
    std::size_t bytes{};
    std::size_t align{};
    bool in_sbo{};

    // allocated case bookkeeping
    std::byte* raw{};
    std::size_t raw_n{};
  };

  // TODO: clean this up to return the block.
  result<void> allocate(block& out, std::size_t bytes, std::size_t align) noexcept {
    out = {};
    out.bytes = bytes;
    out.align = align;

    // SBO path
    if (bytes <= SboBytes && align <= SboAlign) {
      out.in_sbo = true;
      out.ptr = static_cast<void*>(sbo_);
      return {{}, ec::ok};
    }

    // Allocated path (aligned within a byte block)
    out.in_sbo = false;

    // TODO: Investigate.  Do not understand (align -1) logic.  Why is this not just align?  Is this a common pattern?  Does it have a name?
    //Note: what is this? 
    // why is this not align-header_size
    const std::size_t slack = align ? (align - 1) : 0;
    const std::size_t need  = bytes + slack + header_size;

    using byte_alloc  = typename traits::template rebind_alloc<std::byte>;
    using byte_traits = std::allocator_traits<byte_alloc>;
    byte_alloc ba(alloc_);

    std::byte* raw = nullptr;

// TODO: consider MVC flag.
// Note: #if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#if defined(__cpp_exceptions)
    try {
      raw = byte_traits::allocate(ba, need);
    } catch (...) {
      return {{}, ec::alloc_failed};
    }
#else
    raw = byte_traits::allocate(ba, need);
    // TODO: explore this. If the allocator signals failure via exceptions, we won't get here, but if it returns nullptr, we can handle it gracefully. If the allocator is well-behaved and never returns nullptr, this check is redundant but harmless.
    // Note: if the allocator throws, it will get converted to terminate.

    if (!raw) return {{}, ec::alloc_failed};
#endif

    void* p = raw + header_size;
    std::size_t space = need - header_size;
    void* aligned = std::align(align, bytes, p, space);

    if (!aligned) {
      byte_traits::deallocate(ba, raw, need);
      return {{}, ec::alloc_failed};
    }

    header h{raw, need};
    
    std::memcpy(static_cast<std::byte*>(aligned) - header_size, &h, header_size);

    out.ptr = aligned;
    out.raw = raw;
    out.raw_n = need;
    return {{}, ec::ok};
  }

  void deallocate(block& b) noexcept {
    if (!b.ptr) return;
    if (b.in_sbo) { b = {}; return; }

    header h{};
    std::memcpy(&h, static_cast<std::byte*>(b.ptr) - header_size, header_size);

    using byte_alloc  = typename traits::template rebind_alloc<std::byte>;
    using byte_traits = std::allocator_traits<byte_alloc>;
    byte_alloc ba(alloc_);
    byte_traits::deallocate(ba, h.raw, h.n);

    b = {};
  }

private:
  struct header {
    std::byte* raw;
    std::size_t n;
  };

  static_assert(std::is_trivially_copyable_v<header>);

  static constexpr std::size_t header_size = sizeof(header);

  [[no_unique_address]] allocator_type alloc_{};
  alignas(SboAlign) std::byte sbo_[SboBytes]{};
};