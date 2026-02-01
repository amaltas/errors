// Copyright (c) 2026 Amaltas Bohra
// SPDX-License-Identifier: MIT
//
// A discriminated union of a value T or an Error, inspired by Rust's Result
// and Go's (value, error) return pattern.

#pragma once

#include <cassert>
#include <memory>
#include <type_traits>
#include <utility>

#include "errors/error.h"

namespace errors {

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)

// Result<T> is a discriminated union that holds either a success value of
// type T or an Error. It is [[nodiscard]] to encourage callers to inspect
// the result.
template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::decay_t<T>, Error>,
                "Result<Error> is not allowed; use Error directly.");
  static_assert(!std::is_reference_v<T>,
                "Result<T&> is not allowed; use a pointer instead.");

 public:
  // Implicit success construction from a T value.
  // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
  Result(const T& val) : has_value_(true) {
    std::construct_at(&value_, val);
  }
  // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
  Result(T&& val) : has_value_(true) {
    std::construct_at(&value_, std::move(val));
  }

  // Implicit failure construction from an Error.
  // The error must be non-nil.
  // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
  Result(Error err) : has_value_(false) {
    assert(err && "Result<T> failure must hold a non-nil Error");
    std::construct_at(&error_, std::move(err));
  }

  ~Result() { destroy(); }

  Result(const Result& other) : has_value_(other.has_value_) {
    if (has_value_) {
      std::construct_at(&value_, other.value_);
    } else {
      std::construct_at(&error_, other.error_);
    }
  }

  auto operator=(const Result& other) -> Result& {
    if (this != &other) {
      // Copy first so *this is unchanged if T's copy ctor throws.
      Result tmp(other);
      destroy();
      has_value_ = tmp.has_value_;
      if (has_value_) {
        std::construct_at(&value_, std::move(tmp.value_));
      } else {
        std::construct_at(&error_, std::move(tmp.error_));
      }
    }
    return *this;
  }

  Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
      : has_value_(other.has_value_) {
    if (has_value_) {
      std::construct_at(&value_, std::move(other.value_));
    } else {
      std::construct_at(&error_, std::move(other.error_));
    }
  }

  auto operator=(Result&& other) noexcept(
      std::is_nothrow_move_constructible_v<T>) -> Result& {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        std::construct_at(&value_, std::move(other.value_));
      } else {
        std::construct_at(&error_, std::move(other.error_));
      }
    }
    return *this;
  }

  [[nodiscard]] auto ok() const noexcept -> bool { return has_value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }

  // Access the value (asserts on failure state).
  [[nodiscard]] auto value() const& -> const T& {
    assert(has_value_ && "value() called on error Result");
    return value_;
  }
  [[nodiscard]] auto value() & -> T& {
    assert(has_value_ && "value() called on error Result");
    return value_;
  }
  [[nodiscard]] auto value() && -> T&& {
    assert(has_value_ && "value() called on error Result");
    return std::move(value_);
  }

  // NOLINTBEGIN(fuchsia-overloaded-operator)
  [[nodiscard]] auto operator*() const& -> const T& { return value(); }
  [[nodiscard]] auto operator*() & -> T& { return value(); }
  [[nodiscard]] auto operator*() && -> T&& { return std::move(*this).value(); }

  [[nodiscard]] auto operator->() const -> const T* {
    assert(has_value_ && "operator-> called on error Result");
    return &value_;
  }
  [[nodiscard]] auto operator->() -> T* {
    assert(has_value_ && "operator-> called on error Result");
    return &value_;
  }
  // NOLINTEND(fuchsia-overloaded-operator)

  // Access the error (asserts on success state).
  [[nodiscard]] auto error() const& -> const Error& {
    assert(!has_value_ && "error() called on success Result");
    return error_;
  }
  [[nodiscard]] auto error() & -> Error& {
    assert(!has_value_ && "error() called on success Result");
    return error_;
  }
  [[nodiscard]] auto error() && -> Error&& {
    assert(!has_value_ && "error() called on success Result");
    return std::move(error_);
  }

 private:
  void destroy() {
    if (has_value_) {
      std::destroy_at(&value_);
    } else {
      std::destroy_at(&error_);
    }
  }

  union {
    T value_;
    Error error_;
  };
  bool has_value_;
};

// NOLINTEND(cppcoreguidelines-pro-type-union-access)

}  // namespace errors
