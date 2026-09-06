// File: tests/test_proxy.cpp

#include <gtest/gtest.h>
#include <memory>
#include <functional>
#include <stdexcept>
#include <expected>
#include <string>
#include <ndof/patterns/proxy.hpp>



using namespace ndof;

// Simple free function
int add(int a, int b) { return a + b; }

// Functor
struct Multiplier {
    int factor;
    int operator()(int x) const { return x * factor; }
};

// Class with member function
struct Accumulator {
    int value = 0;
    int add(int x) { value += x; return value; }
    int get() const { return value; }
};

// // Test for function pointer proxy
// TEST(ProxyTest, FunctionPointer) {
//     Proxy<decltype(&add)> proxy(&add);
//     auto result = proxy(2, 3);
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result.value(), 5);
// }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}