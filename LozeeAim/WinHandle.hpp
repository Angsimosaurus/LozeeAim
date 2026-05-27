#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : handle(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const { return handle; }
    bool is_valid() const { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return is_valid(); }

    HANDLE release() {
        HANDLE value = handle;
        handle = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) {
        if (is_valid()) {
            CloseHandle(handle);
        }
        handle = value;
    }

private:
    HANDLE handle = nullptr;
};
