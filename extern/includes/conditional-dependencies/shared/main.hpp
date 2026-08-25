#pragma once

#include <dlfcn.h>
#include <optional>
#include <string_view>
#include <vector>
#include <type_traits>
#include <typeinfo>
#include <string>
#include "utils.hpp"

namespace CondDeps {

    #ifndef EXPOSE_API
    // Exposes an API method of this signature for other mods to use in a conditional and type safe fashion.
    #define EXPOSE_API(name, retVal, ...) \
    retVal name(__VA_ARGS__); \
    extern "C" ::CondDeps::CondDepsRet  __ ##name() { \
        return ::CondDeps::CondDepConverter<decltype(&name)>::get(&name); \
    } \
    retVal name(__VA_ARGS__)
    #endif

    #ifndef EXPOSE_STATIC_API
    // Exposes an API method of this signature that wraps the provided method pointer to use in a conditional and type safe fashion.
    #define EXPOSE_STATIC_API(name, ptr) \
    extern "C" ::CondDeps::CondDepsRet __ ##name() { \
        return ::CondDeps::CondDepConverter<decltype(ptr)>::get(ptr); \
    }
    #endif

    #ifndef EXPOSE_INSTANCE_API
    // Exposes an API method of this signature that wraps the provided instance member pointer to use in a conditional and type safe fashion.
    #define EXPOSE_INSTANCE_API(name, ptr) \
    extern "C" ::CondDeps::CondDepsRet __ ##name() { \
        return ::CondDeps::CondDepConverter<typename ::CondDeps::FunctionWrapper<decltype(ptr)>::WrapperType>::get(&::CondDeps::FunctionWrapper<decltype(ptr)>::wrap<ptr>); \
    }
    #endif

    template<class Ret = void*>
    std::optional<Ret> GetSymbol(std::string_view id, std::string_view name) {
        std::string partial("lib" + std::string(id) + ".so");
        auto searchPath = CondDeps::Internal::cond_getPath() + partial;
        if (!CondDeps::Internal::cond_fileexists(searchPath)) {
            return std::nullopt;
        }
        // Clear dlerror first
        dlerror();
        auto* handle = dlopen(searchPath.c_str(), RTLD_LOCAL | RTLD_NOW);
        auto* err = dlerror();
        if (handle == nullptr || err != nullptr) {
            return std::nullopt;
        }
        auto* sym = dlsym(handle, (std::string("__") + name.data()).c_str());
        err = dlerror();
        if (sym == nullptr || err != nullptr) {
            return std::nullopt;
        }
        return sym;
    }

    /// @brief Finds a method pointer from the provided module ID and name.
    /// The method pointer should be exposed in the target module with an EXPOSE_API call.
    /// This method will return std::nullopt if:
    /// - The target module/method does not exist/there was an error opening the target module
    /// - The target method does not have the correct return type and parameter type names and sizes
    /// Otherwise, will return a method pointer to target.
    /// @tparam R The return type of the function to cast to.
    /// @tparam TArgs The argument types of the function to cast to.
    /// @param id The module id to find the method on.
    /// @param name The name of the method to find.
    /// @return The method pointer, if found and type checked.
    template<class R, class... TArgs>
    std::optional<R (*)(TArgs...)> Find(std::string_view id, std::string_view name) {
        auto sym = GetSymbol(id, name);
        if (!sym) {
            return std::nullopt;
        }
        // If we found a symbol, cast it to our function pointer type and attempt to read it
        auto res = reinterpret_cast<CondDepsRet (*)()>(*sym)();
        if (!CondDepConverter<R (*)(TArgs...)>::match(res)) {
            // We failed to match, we have already logged.
            return std::nullopt;
        }
        // We successfully matched, return the function pointer.
        return reinterpret_cast<R (*)(TArgs...)>(res.func);
    }

    /// @brief Same as Find, except only checks the size of types for validity, instead of their full type name.
    /// Note that the size of a reference type is the size of the type is is a reference of, so be cautious.
    /// @tparam R The return type of the function to cast to.
    /// @tparam TArgs The argument types of the function to cast to.
    /// @param id The module id to find the method on.
    /// @param name The name of the method to find.
    /// @return The method pointer, if found and type checked (size check only).
    template<class R, class... TArgs>
    std::optional<R (*)(TArgs...)> FindUnsafe(std::string_view id, std::string_view name) {
        auto sym = GetSymbol(id, name);
        if (!sym) {
            return std::nullopt;
        }
        // If we found a symbol, cast it to our function pointer type and attempt to read it
        auto res = reinterpret_cast<CondDepsRet (*)()>(*sym)();
        if (!CondDepConverter<R (*)(TArgs...)>::matchUnsafe(res)) {
            // We failed to match, we have already logged.
            return std::nullopt;
        }
        // We successfully matched, return the function pointer.
        return reinterpret_cast<R (*)(TArgs...)>(res.func);
    }
}