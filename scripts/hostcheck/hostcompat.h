#pragma once
// Bionic's <sys/types.h> defines these; glibc does not. beatsaber-hook's
// typedefs-list.hpp uses uint_t unqualified.
#include <cstdint>
typedef unsigned int uint_t;
typedef unsigned long ulong_t;
