// Copyright (c) 2026 Amaltas Bohra
// SPDX-License-Identifier: MIT
//
// A lightweight, high-performance error handling library for C++23 with
// first-class support for error chaining, identity preservation, and
// structured payloads.

#pragma once

#include <bit>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace errors {

class Error;

// Forward declarations for internal types
namespace internal {
class ErrorImpl;
}  // namespace internal

// Thread safety:
//   Error is safe for concurrent read access from multiple threads.
//   Copies share state via atomic reference counting; reading from
//   multiple threads is safe. Concurrent read+write to the SAME
//   Error object is NOT safe (same as std::string or absl::Status).
//   Non-const As<T>() triggers copy-on-write and mutates the handle.

// Core Error Class
// A lightweight handle to a polymorphic error implementation.
// 8 bytes (tagged pointer). Encoding:
//   bits_ == 0        → Nil
//   bits_ & 1 (odd)   → Sentinel (non-owning, pointer = bits_ & ~1)
//   bits_ even, != 0  → Dynamic  (owning, pointer = bits_)
class Error {
 private:
  uintptr_t bits_ = 0;

  [[nodiscard]] auto is_nil() const noexcept -> bool { return bits_ == 0; }
  [[nodiscard]] auto is_sentinel() const noexcept -> bool {
    return (bits_ & 1) != 0;
  }
  [[nodiscard]] auto is_dynamic() const noexcept -> bool {
    return bits_ != 0 && (bits_ & 1) == 0;
  }

  // Private constructor for dynamic errors (takes ownership).
  explicit Error(internal::ErrorImpl* impl) noexcept
      : bits_(std::bit_cast<uintptr_t>(impl)) {}

  // Internal helpers to get the impl pointer regardless of tag.
  [[nodiscard]] auto get_impl() const noexcept -> const internal::ErrorImpl*;
  [[nodiscard]] auto get_mutable_impl() noexcept -> internal::ErrorImpl*;

  // Internal method to walk the error chain.
  [[nodiscard]] auto unwrap() const noexcept -> const Error*;

 public:
  // Default constructor creates a Nil error.
  constexpr Error() noexcept = default;

  // Sentinel constructor (used by ERRORS_DEFINE_SENTINEL macro).
  explicit Error(const internal::ErrorImpl* static_impl) noexcept
      : bits_(std::bit_cast<uintptr_t>(static_impl) | 1) {}

  // Destructor, copy/move operations (defined in .cc).
  ~Error();
  Error(const Error& other);
  auto operator=(const Error& other) -> Error&;
  Error(Error&& other) noexcept;
  auto operator=(Error&& other) noexcept -> Error&;

  // Returns true if the error is not nil.
  [[nodiscard]] explicit operator bool() const noexcept;

  // Returns the full error message, including wrapped context.
  [[nodiscard]] auto message() const -> std::string;

  // Returns the message for this single layer (no chain traversal).
  // Returns empty string_view for nil errors.
  [[nodiscard]] auto what() const noexcept -> std::string_view;

  // Returns a pointer to the next error in the chain, or nullptr.
  [[nodiscard]] auto Unwrap() const noexcept -> const Error*;

  // Returns the global singleton instance of a Nil error.
  [[nodiscard]] static auto Nil() noexcept -> const Error&;

  // Friend Declarations

  template <typename... Args>
  friend auto Errorf(const std::format_string<Args...>& fmt, Args&&... args)
      -> Error;

  template <typename... Args>
  friend auto Wrapf(Error inner, const std::format_string<Args...>& fmt,
                    Args&&... args) -> Error;

  template <typename T, typename... Args>
  friend auto WrapWithPayload(std::in_place_type_t<T>, Error inner,
                              std::string_view msg, Args&&... args) -> Error;

  template <typename T>
  friend auto WrapWithPayload(Error inner, std::string_view msg, T&& payload)
      -> Error;

  template <typename T, typename... Args>
  friend auto NewWithPayload(std::in_place_type_t<T>, std::string_view msg,
                             Args&&... args) -> Error;

  template <typename T>
  friend auto NewWithPayload(std::string_view msg, T&& payload) -> Error;

  friend auto New(std::string_view msg) -> Error;
  friend auto Wrap(Error inner, std::string_view msg) -> Error;
  // NOLINTNEXTLINE(fuchsia-overloaded-operator)
  friend auto operator==(const Error& lhs, const Error& rhs) noexcept -> bool;
  friend auto Is(const Error& err, const Error& target) -> bool;

  friend auto IsSerializable(const Error& err) -> bool;
  friend auto DebugString(const Error& err) -> std::string;
  friend auto Serialize(const Error& err) -> std::string;
  friend auto Deserialize(std::string_view data) -> Error;

  template <typename T>
  friend auto As(Error& err) -> T*;
  template <typename T>
  friend auto As(const Error& err) -> const T*;
};

// Comparison Operators
// NOLINTNEXTLINE(fuchsia-overloaded-operator)
[[nodiscard]] auto operator==(const Error& lhs, const Error& rhs) noexcept
    -> bool;

// Non-template free function declarations
// Friend declarations inside the class inject these names into the namespace,
// but an explicit declaration is required for unqualified lookup in some
// compilers.
[[nodiscard]] auto New(std::string_view msg) -> Error;
[[nodiscard]] auto Wrap(Error inner, std::string_view msg) -> Error;
[[nodiscard]] auto Is(const Error& err, const Error& target) -> bool;

// Serialization support.
// IsSerializable returns true if every layer in the chain is a dynamic error
// (not a sentinel) and every payload satisfies the WireSerializable concept.
[[nodiscard]] auto IsSerializable(const Error& err) -> bool;

// DebugString returns the full error chain message with inline payload debug
// info. For layers carrying a WireSerializable payload, the type name and
// debug string are appended in brackets: "msg [type: debug_info]: ...".
[[nodiscard]] auto DebugString(const Error& err) -> std::string;

// Serialize encodes the error chain into a binary format suitable for logging,
// storage, or wire transport. Requires IsSerializable(err) to be true;
// non-serializable layers are silently skipped.
[[nodiscard]] auto Serialize(const Error& err) -> std::string;

// Deserialize reconstructs an error chain from bytes produced by Serialize.
// Payloads arrive as SerializedPayload -- use As<SerializedPayload>(err) to
// access the type URL and raw bytes, then parse with the appropriate type.
[[nodiscard]] auto Deserialize(std::string_view data) -> Error;

// Deserialized payload container.
// When an Error is deserialized via Deserialize(), payloads that were
// originally protobuf messages (or any WireSerializable type) arrive as
// SerializedPayload. Use As<SerializedPayload>(err) to access the raw type
// URL and bytes, then parse with the appropriate message type.
struct SerializedPayload {
  std::string type_url;  // NOLINT
  std::string data;      // NOLINT

  // WireSerializable interface for re-serialization and debug output.
  [[nodiscard]] auto SerializeAsString() const -> std::string { return data; }
  [[nodiscard]] auto GetTypeName() const -> std::string { return type_url; }
  [[nodiscard]] auto ShortDebugString() const -> std::string {
    return "(" + std::to_string(data.size()) + " bytes)";
  }
};

// Sentinel Error Definition Macro
// Defines a static, compile-time-initialized sentinel error.
// Uses inline constinit to avoid ODR violations (no anonymous namespace).
#define ERRORS_DEFINE_SENTINEL(name, msg)                                   \
  inline constinit const errors::internal::StaticSentinelError name##_impl( \
      msg);                                                                 \
  inline const errors::Error name(&name##_impl)

}  // namespace errors

// Template Implementation Section
// We use the "Lock and Key" pattern to include the internal implementation
// header.
#define ERRORS_PUBLIC_HEADER_INCLUDED_
#include "errors/error_internal.h"
#undef ERRORS_PUBLIC_HEADER_INCLUDED_

namespace errors {

// Definitions for template functions declared as friends above.

template <typename... Args>
[[nodiscard]] auto Errorf(const std::format_string<Args...>& fmt,
                          Args&&... args) -> Error {
  return New(std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
[[nodiscard]] auto Wrapf(Error inner, const std::format_string<Args...>& fmt,
                         Args&&... args) -> Error {
  auto msg = std::format(fmt, std::forward<Args>(args)...);
  return Error(new internal::DynamicError(msg, std::move(inner)));
}

template <typename T>
[[nodiscard]] auto WrapWithPayload(Error inner, std::string_view msg,
                                   T&& payload) -> Error {
  return Error(new internal::DetailedError<std::decay_t<T>>(
      msg, std::forward<T>(payload), std::move(inner)));
}

template <typename T, typename... Args>
[[nodiscard]] auto WrapWithPayload(std::in_place_type_t<T>, Error inner,
                                   std::string_view msg, Args&&... args)
    -> Error {
  return Error(new internal::DetailedError<T>(
      msg, T{std::forward<Args>(args)...}, std::move(inner)));
}

template <typename T>
[[nodiscard]] auto NewWithPayload(std::string_view msg, T&& payload) -> Error {
  return WrapWithPayload(Error::Nil(), msg, std::forward<T>(payload));
}

template <typename T, typename... Args>
[[nodiscard]] auto NewWithPayload(std::in_place_type_t<T>, std::string_view msg,
                                  Args&&... args) -> Error {
  return WrapWithPayload(std::in_place_type<T>, Error::Nil(), msg,
                         std::forward<Args>(args)...);
}

template <typename T>
[[nodiscard]] auto As(Error& err) -> T* {
  // Const search first to avoid unnecessary COW when payload is absent.
  if (As<T>(static_cast<const Error&>(err)) == nullptr) {
    return nullptr;
  }
  for (Error* current = &err; current != nullptr;
       current = const_cast<Error*>(current->unwrap())) {  // NOLINT
    // NOLINTNEXTLINE
    if (auto* impl = current->get_mutable_impl()) {
      if (auto* payload = impl->get_payload(internal::type_id<T>())) {
        return static_cast<T*>(payload);
      }
    }
  }
  return nullptr;
}

template <typename T>
[[nodiscard]] auto As(const Error& err) -> const T* {
  for (const Error* current = &err; current != nullptr;
       current = current->unwrap()) {
    if (auto* impl = current->get_impl()) {  // NOLINT
      if (auto* payload = impl->get_payload(internal::type_id<T>())) {
        return static_cast<const T*>(payload);
      }
    }
  }
  return nullptr;
}

}  // namespace errors

// Macro: ERRORS_RETURN_IF_ERROR(expr)
// Evaluates `expr` (must yield an Error or type convertible to bool).
// If the result is non-nil, returns it immediately from the enclosing function.
// Works for functions returning Error or Result<void>.
#define ERRORS_RETURN_IF_ERROR(expr) \
  do {                               \
    if (auto _err_ = (expr); _err_) {\
      return _err_;                  \
    }                                \
  } while (false)

// Macro: ERRORS_RETURN_IF_ERROR_WRAPF(expr, fmt, ...)
// Evaluates `expr` (must yield an Error). If non-nil, wraps the error with a
// formatted message via errors::Wrapf and returns it from the enclosing
// function. This avoids the common bug of composing Wrap inside the plain
// macro, which would wrap nil errors into non-nil ones.
#define ERRORS_RETURN_IF_ERROR_WRAPF(expr, ...)            \
  do {                                                      \
    if (auto _err_ = (expr); _err_) {                      \
      return errors::Wrapf(std::move(_err_), __VA_ARGS__); \
    }                                                       \
  } while (false)

template <>
struct std::formatter<errors::Error> {
  static constexpr auto parse(std::format_parse_context& ctx) {
    return ctx.begin();
  }
  static auto format(const errors::Error& err, std::format_context& ctx) {
    return std::format_to(ctx.out(), "{}", err.message());
  }
};
