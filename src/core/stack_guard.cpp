// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/stack_guard.hpp"

#include <string>

#include "script/error.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__) || defined(__unix__)
#include <pthread.h>
#endif

namespace script::internal {

thread_local std::uintptr_t nativeStackFloorCache = 0;

namespace {

/// The guard page and the red zone below the usable stack are not for the program to use.
constexpr std::uintptr_t kGuardMargin = 16 * 1024;

std::uintptr_t lookup() noexcept
{
#if defined(_WIN32)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return low ? std::uintptr_t(low) + kGuardMargin : 1;
#elif defined(__APPLE__)
    const pthread_t self = pthread_self();
    const std::uintptr_t high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
    const std::size_t size = pthread_get_stacksize_np(self);
    return high && size && high > size ? high - size + kGuardMargin : 1;
#elif defined(__linux__) || defined(__unix__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return 1;
    void* base = nullptr;
    std::size_t size = 0;
    const int rc = pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || !base || size == 0)
        return 1;
    return reinterpret_cast<std::uintptr_t>(base) + kGuardMargin;
#else
    return 1;
#endif
}

}  // namespace

void throwTooDeep(const char* what)
{
    throw Error(ErrorCode::CompileError, std::string(what) + " is nested too deeply");
}

std::uintptr_t nativeStackFloorLookup() noexcept
{
    const std::uintptr_t floor = lookup();
    nativeStackFloorCache = floor;
    return floor;
}

}  // namespace script::internal
