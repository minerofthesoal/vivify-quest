#include "main.hpp"

// A redefinition of A should still be permissible.
struct A {
    int x;
};

// If this does not crash, it performs as expected.
// See test.cpp to see the definition of the test function.
extern "C" void load() {
    auto test = CondDeps::Find<void, int, float>("conditional-dependencies-test", "test");
    auto cool = CondDeps::Find<void, int, int&>("conditional-dependencies-test", "somethingCool");
    auto inst = CondDeps::Find<int, A*>("conditional-dependencies-test", "A_get_x");
    auto unsafeInst = CondDeps::FindUnsafe<int, int*>("conditional-dependencies-test", "A_get_x");
    test.value()(3, 4.1f);
    int someInt = 2442;
    cool.value()(42, someInt);
    A a{12};
    auto val = inst.value()(&a);
    if (val != 12) {
        std::terminate();
    }
    // A is sizeof(int), A* is simply just an int*, get_x should return the int at the location pointed to.
    // Thus, this should work and be found/type checked.
    if (2442 != unsafeInst.value()(&someInt)) {
        std::terminate();
    }
}