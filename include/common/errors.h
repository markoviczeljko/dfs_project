#ifndef DFS_COMMON_ERRORS_H_
#define DFS_COMMON_ERRORS_H_

#include <cassert>
#include <cstdint>
#include <utility>
#include <variant>

enum class ErrorCode : uint16_t {
    OK,
    INVALID_REQUEST,
    INVALID_MESSAGE,
    FILE_NOT_FOUND,
    FILE_ALREADY_EXISTS,
    BLOCK_NOT_FOUND,
    NODE_NOT_FOUND,
    BLOCK_CORRUPTED,
    STORAGE_FULL,
    TIMEOUT,
    INTERNAL_ERROR
};

// Hand-rolled stand-in for std::expected<T, ErrorCode> (C++23-only).
// Mirrors the subset of its API we need so the project can drop-in
// upgrade to std::expected if it ever moves to C++23. See docs/design.md.
template <typename T>
class Result {
public:
    static Result ok(T value) { return Result(std::move(value)); }
    static Result err(ErrorCode error) { return Result(error); }

    bool is_ok() const { return std::holds_alternative<T>(storage_); }
    bool is_error() const { return !is_ok(); }
    explicit operator bool() const { return is_ok(); }

    const T& value() const {
        assert(is_ok());
        return std::get<T>(storage_);
    }

    T& value() {
        assert(is_ok());
        return std::get<T>(storage_);
    }

    ErrorCode error() const {
        assert(is_error());
        return std::get<ErrorCode>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(ErrorCode error) : storage_(error) {}

    std::variant<T, ErrorCode> storage_;
};

#endif  // DFS_COMMON_ERRORS_H_
