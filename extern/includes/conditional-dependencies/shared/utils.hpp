#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cxxabi.h>
#include <vector>
#include <array>
#include "logging.hpp"
#include "modloader/shared/modloader.hpp"

namespace CondDeps {
    extern "C" {
        /// @brief Type Info wrapper.
        struct TInfo {
            std::string name;
            size_t size;
            bool equals(TInfo const& other) {
                return name == other.name && size == other.size;
            }
            // bool operator==(TInfo const& other) const {
            //     return name == other.name && size == other.size;
            // }
        };
        /// @brief The return instance of a conditional types extern "C" function.
        struct CondDepsRet {
            TInfo ret;
            std::vector<TInfo> params;
            void* func;
        };
    }

    /// @brief Returns the converted type name of the provided type.
    /// @tparam T The type to get the converted type name of.
    template<typename T>
    static std::string typeName() {
        if constexpr (!std::is_reference_v<T>) {
            int status;
            auto* demangle = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
            if (status) {
                return typeid(T).name();
            }
            std::string ret(demangle);
            free(demangle);
            return ret;
        } else {
            int status;
            auto* demangle = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
            if (status) {
                return std::string(typeid(T).name()) + "&";
            }
            std::string ret(demangle);
            free(demangle);
            return ret + "&";
        }
    }

    template<class T>
    struct CondDepConverter;

    /// @brief Provides conversions from a method pointer into various conddep components.
    /// @tparam R The return type
    /// @tparam TArgs The parameter types
    template<class R, class... TArgs>
    struct CondDepConverter<R (*)(TArgs...)> {
        static std::vector<TInfo> params() {
            return {TInfo{typeName<TArgs>(), sizeof(TArgs)}...};
        }
        static TInfo retType() {
            if constexpr (std::is_same_v<R, void>) {
                return {"void", 0};
            } else {
                return {typeName<R>(), sizeof(R)};
            }
        }
        static CondDepsRet get(R (*func)(TArgs...)) {
            return {retType(), params(), reinterpret_cast<void*>(func)};
        }

        static bool match(CondDepsRet& ret) {
            auto rt = retType();
            if (!ret.ret.equals(rt)) {
                // Expected: ret.retName but got: retName()!
                CONDDEPS_LOG_WARN("Return type mismatch! Expected: %s (size: %zu), but got: %s (size: %zu)!", ret.ret.name.c_str(), ret.ret.size, rt.name.c_str(), rt.size);
                return false;
            }
            if (ret.params.size() != sizeof...(TArgs)) {
                // Expected: ret.params.size() but got: sizeof...(TArgs)!
                CONDDEPS_LOG_WARN("Parameter count mismatch! Expected: %zu, but got: %zu!", ret.params.size(), sizeof...(TArgs));
                return false;
            }
            std::array<TInfo, sizeof...(TArgs)> arr{TInfo{typeName<TArgs>(), sizeof(TArgs)}...};
            
            for (size_t i = 0; i < sizeof...(TArgs); ++i) {
                auto p = ret.params[i];
                auto actual = arr[i];
                if (!p.equals(actual)) {
                    // Parameter at: i expected: ret.params[i] but got: arr[i]!
                    CONDDEPS_LOG_WARN("Parameter mismatch! Parameter: %zu expected: %s (size: %zu), but got: %s (size: %zu)!", i, p.name.c_str(), p.size, actual.name.c_str(), actual.size);
                    return false;
                }
            }
            return true;
        }

        // Match only sizes, not types
        static bool matchUnsafe(CondDepsRet& ret) {
            auto rt = retType();
            if (ret.ret.size != rt.size) {
                // Expected: ret.retName but got: retName()!
                CONDDEPS_LOG_WARN("Return type mismatch! Expected size: %zu but got size: %zu!", ret.ret.size, rt.size);
                return false;
            }
            if (ret.params.size() != sizeof...(TArgs)) {
                // Expected: ret.params.size() but got: sizeof...(TArgs)!
                CONDDEPS_LOG_WARN("Parameter count mismatch! Expected: %zu but got: %zu!", ret.params.size(), sizeof...(TArgs));
                return false;
            }
            std::array<TInfo, sizeof...(TArgs)> arr{TInfo{typeName<TArgs>(), sizeof(TArgs)}...};
            
            for (size_t i = 0; i < sizeof...(TArgs); ++i) {
                auto p = ret.params[i];
                if (p.size != arr[i].size) {
                    // Parameter at: i expected: ret.params[i] but got: arr[i]!
                    CONDDEPS_LOG_WARN("Parameter mismatch! Parameter: %zu expected size: %zu, but got size: %zu!", i, p.size, arr[i].size);
                    return false;
                }
            }
            return true;
        }
    };

    template<class T>
    struct FunctionWrapper;

    template<class R, class T, class... TArgs>
    struct FunctionWrapper<R (T::*)(TArgs...)> {
        using WrapperType = R (*)(T*, TArgs...);
        template<R (T::*memb)(TArgs...)>
        static R wrap(T* self, TArgs... args) {
            if constexpr (std::is_same_v<R, void>) {
                (self->*memb)(args...);
            } else {
                return (self->*memb)(args...);
            }
        }
    };

    struct Internal {
        static bool cond_fileexists(std::string_view filename) {
            return access(filename.data(), W_OK | R_OK) != -1;
        }

        template<typename... TArgs>
        static std::string cond_string_format(const std::string_view format, TArgs ... args)
        {
        #pragma GCC diagnostic ignored "-Wformat-security"
        #pragma GCC diagnostic push
            size_t size = snprintf(nullptr, 0, format.data(), args ...) + 1; // Extra space for '\0'
            if (size <= 0)
                return "";
            std::unique_ptr<char[]> buf(new char[size]); 
            snprintf(buf.get(), size, format.data(), args...);
            return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
        #pragma GCC diagnostic pop
        }

        static std::string cond_getPath() {
            static auto path = Modloader::getDestinationPath();
            return path;
        }
    };
}
