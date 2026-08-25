#include "main.hpp"

void funcILike(int one, int& two) {
}


struct A {
    int x;
    int get_x() {
        return x;
    }
};

#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
#pragma GCC diagnostic push
EXPOSE_API(test, void, int x, float y) {
    // do something
}

EXPOSE_STATIC_API(somethingCool, &funcILike)

EXPOSE_INSTANCE_API(A_get_x, &A::get_x);
#pragma GCC diagnostic pop

extern "C" void load() {
    // Do nothing
}