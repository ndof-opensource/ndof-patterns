// Copyright 2026 The ndof Authors
// SPDX-License-Identifier: Apache-2.0

#include "ndof/patterns/version.hpp"

#include <gtest/gtest.h>

namespace {

TEST(Version, LibraryNameMatchesPackage) {
    EXPECT_EQ(ndof::patterns::library_name(), "ndof-patterns");
}

TEST(Version, LibraryVersionIsNonEmpty) {
    EXPECT_FALSE(ndof::patterns::library_version().empty());
}

} // namespace
