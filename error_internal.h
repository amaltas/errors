// Copyright (c) 2026 Amaltas Bohra
// SPDX-License-Identifier: MIT
//
// This is a private internal header containing the implementation details
// of the errors library. It is not intended for direct inclusion by
// users.

#ifndef ERRORS_PUBLIC_HEADER_INCLUDED_
#include "errors/error.h"
#else

#ifndef ERRORS_ERROR_INTERNAL_H_
#define ERRORS_ERROR_INTERNAL_H_

#include <atomic>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace errors {
class Error;
}  // namespace errors

namespace errors::internal {

// Compile-time type identity without RTTI.
// Each instantiation has a unique static address used as an ID.
using TypeId = const void*;
template <typename T>
auto type_id() noexcept -> TypeId {
  static constexpr char tid = '\0';
  return &tid;
}

// Concept that matches types with a protobuf-like serialization interface.
// Any type providing SerializeAsString() and GetTypeName() qualifies,
// including all google::protobuf::Message subclasses.
template <typename T>
concept WireSerializable = requires(const T& obj) {
  { obj.SerializeAsString() } -> std::convertible_to<std::string>;
  { obj.GetTypeName() } -> std::convertible_to<std::string>;
};

// Optional concept for human-readable debug output.
// Matches types providing ShortDebugString() (e.g., protobuf messages).
template <typename T>
concept HasDebugString = requires(const T& obj) {
  { obj.ShortDebugString() } -> std::convertible_to<std::string>;
};

// Base Interface
// The polymorphic base for all concrete error implementations.
class ErrorImpl {
 public:
  ErrorImpl() = default;
  virtual ~ErrorImpl() = default;

  ErrorImpl(const ErrorImpl&) = delete;
  auto operator=(const ErrorImpl&) -> ErrorImpl& = delete;
  ErrorImpl(ErrorImpl&&) = delete;
  auto operator=(ErrorImpl&&) -> ErrorImpl& = delete;

  // Returns the error message for this specific layer (no chain traversal).
  [[nodiscard]] virtual auto message_view() const noexcept
      -> std::string_view = 0;

  // Returns a pointer to the wrapped error, if any.
  [[nodiscard]] virtual auto unwrap() const noexcept -> const Error* {
    return nullptr;
  }

  // Type-safe payload extraction. Overridden by DetailedError.
  [[nodiscard]] virtual auto get_payload(TypeId) noexcept -> void* {
    return nullptr;
  }
  [[nodiscard]] virtual auto get_payload(TypeId) const noexcept -> const void* {
    return nullptr;
  }

  // Extensible matching hook for Is(). Default returns false.
  [[nodiscard]] virtual auto matches(const Error&) const noexcept -> bool {
    return false;
  }

  // Serialization support. Overridden by DynamicError and DetailedError.
  [[nodiscard]] virtual auto is_serializable() const noexcept -> bool {
    return false;
  }
  [[nodiscard]] virtual auto serialize_payload() const -> std::string {
    return {};
  }
  [[nodiscard]] virtual auto payload_type_url() const -> std::string {
    return {};
  }
  [[nodiscard]] virtual auto payload_debug_string() const -> std::string {
    return {};
  }

  // Deep-clone this error. Only meaningful for dynamic errors.
  // Sentinels return nullptr (they are never cloned).
  [[nodiscard]] virtual auto clone() const -> ErrorImpl* { return nullptr; }

  void add_ref() const noexcept {
    ref_count_.fetch_add(1, std::memory_order_relaxed);
  }
  auto release() const noexcept -> bool {
    return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }
  [[nodiscard]] auto use_count() const noexcept -> uint32_t {
    return ref_count_.load(std::memory_order_relaxed);
  }

 private:
  // Intrusive reference counting (atomic for thread safety).
  mutable std::atomic<uint32_t> ref_count_{1};
};

// Dynamic Error Implementation
// Handles runtime-generated errors with SSO (Small String Optimization).
class DynamicError : public ErrorImpl {
 private:
  static constexpr size_t SSO_CAPACITY = 23;
  union {
    char sso_buffer_[SSO_CAPACITY + 1];  // NOLINT
    char* heap_buffer_;
  } data_{};
  size_t size_{};

  // Direct storage — no unique_ptr indirection.
  Error inner_;

  void set_string(std::string_view msg);
  [[nodiscard]] auto is_sso() const noexcept -> bool {
    return size_ <= SSO_CAPACITY;
  }

 public:
  explicit DynamicError(std::string_view msg);
  DynamicError(std::string_view msg, Error inner);
  ~DynamicError() override;

  // Rule of 5: Required due to manual union management.
  DynamicError(const DynamicError&);
  auto operator=(const DynamicError&) -> DynamicError&;
  DynamicError(DynamicError&&) noexcept;
  auto operator=(DynamicError&&) noexcept -> DynamicError&;

  [[nodiscard]] auto message_view() const noexcept -> std::string_view override;
  [[nodiscard]] auto unwrap() const noexcept -> const Error* override;
  [[nodiscard]] ErrorImpl* clone() const override;
  [[nodiscard]] bool is_serializable() const noexcept override { return true; }
};

// Sentinel Implementation
// A lightweight implementation for compile-time constant errors.
class StaticSentinelError : public ErrorImpl {
 public:
  consteval StaticSentinelError(const char* msg) : msg_(msg) {}
  [[nodiscard]] std::string_view message_view() const noexcept override {
    return msg_;
  }

 private:
  const char* const msg_;
};

// Detailed Error with Payload
// A template class that stores a custom user-defined payload.
template <typename T>
class DetailedError final : public DynamicError {
 public:
  // Constructs a DetailedError by moving or copying a payload and an inner
  // error.
  template <typename U>
  DetailedError(std::string_view msg, U&& details, Error inner)
      : DynamicError(msg, std::move(inner)),
        details_(std::forward<U>(details)) {}

  [[nodiscard]] void* get_payload(TypeId id) noexcept override {
    if (id == type_id<T>()) return &details_;
    return nullptr;
  }

  [[nodiscard]] const void* get_payload(TypeId id) const noexcept override {
    if (id == type_id<T>()) return &details_;
    return nullptr;
  }

  [[nodiscard]] ErrorImpl* clone() const override {
    static_assert(
        std::is_copy_constructible_v<T>,
        "DetailedError<T>::clone() requires T to be copy-constructible. "
        "Move-only types cannot be used as error payloads because Error "
        "uses copy-on-write semantics.");
    return new DetailedError(*this);
  }

  [[nodiscard]] bool is_serializable() const noexcept override {
    return WireSerializable<T>;
  }

  [[nodiscard]] std::string serialize_payload() const override {
    if constexpr (WireSerializable<T>) {
      return details_.SerializeAsString();
    }
    return {};
  }

  [[nodiscard]] std::string payload_type_url() const override {
    if constexpr (WireSerializable<T>) {
      return details_.GetTypeName();
    }
    return {};
  }

  [[nodiscard]] std::string payload_debug_string() const override {
    if constexpr (HasDebugString<T>) {
      return details_.ShortDebugString();
    } else if constexpr (WireSerializable<T>) {
      return "(" + std::to_string(details_.SerializeAsString().size()) +
             " bytes)";
    }
    return {};
  }

 private:
  T details_;
};

}  // namespace errors::internal

#endif  // ERRORS_ERROR_INTERNAL_H_
#endif  // ERRORS_PUBLIC_HEADER_INCLUDED_
