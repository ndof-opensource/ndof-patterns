// Copyright 2026 The ndof Authors
// SPDX-License-Identifier: Apache-2.0

#include "ndof/patterns/version.hpp"

#include <string_view>

namespace ndof::patterns {

std::string_view library_name() noexcept {
    return NDOF_LIBRARY_NAME;
}

std::string_view library_version() noexcept {
    return NDOF_LIBRARY_VERSION;
}

} // namespace ndof::patterns
