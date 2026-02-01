// Copyright (c) 2026 Amaltas Bohra
// SPDX-License-Identifier: MIT
//
// Implementation of the non-template components of the errors library.

#include "errors/error.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace errors {

// Guarantee that ErrorImpl pointers are at least 2-byte aligned so the low
// bit is always free for tagging.
static_assert(alignof(internal::ErrorImpl) >= 2);

// Error Class Implementation

Error::~Error() {
  if (is_dynamic()) {
    auto* impl = std::bit_cast<internal::ErrorImpl*>(bits_);
    if (impl->release()) {
      delete impl;  // NOLINT
    }
  }
}

Error::Error(const Error& other) : bits_(other.bits_) {
  if (is_dynamic()) {
    std::bit_cast<internal::ErrorImpl*>(bits_)->add_ref();
  }
}

auto Error::operator=(const Error& other) -> Error& {
  if (this != &other) {
    Error tmp(other);  // NOLINT(misc-const-correctness)
    *this = std::move(tmp);
  }
  return *this;
}

Error::Error(Error&& other) noexcept : bits_(other.bits_) { other.bits_ = 0; }

auto Error::operator=(Error&& other) noexcept -> Error& {
  if (this != &other) {
    if (is_dynamic()) {
      auto* impl = std::bit_cast<internal::ErrorImpl*>(bits_);
      if (impl->release()) {
        delete impl;  // NOLINT
      }
    }
    bits_ = other.bits_;
    other.bits_ = 0;
  }

  return *this;
}

auto Error::get_impl() const noexcept -> const internal::ErrorImpl* {
  return std::bit_cast<const internal::ErrorImpl*>(bits_ & ~uintptr_t{1});
}

auto Error::get_mutable_impl() noexcept -> internal::ErrorImpl* {
  if (!is_dynamic()) {
    return nullptr;
  }
  auto* impl = std::bit_cast<internal::ErrorImpl*>(bits_);
  if (impl->use_count() > 1) {
    // Copy-on-write: clone the outermost layer, release old ref.
    auto* copy = impl->clone();
    impl->release();
    bits_ = std::bit_cast<uintptr_t>(copy);
    return copy;
  }

  return impl;
}

auto Error::unwrap() const noexcept -> const Error* {
  const auto* impl = get_impl();
  return impl != nullptr ? impl->unwrap() : nullptr;
}

Error::operator bool() const noexcept { return bits_ != 0; }

auto Error::message() const -> std::string {
  if (bits_ == 0) {
    return "(nil)";
  }

  // Two-pass: first compute total length, then fill pre-reserved string.
  // Eliminates reallocations on deep chains.
  size_t total_len = 0;
  size_t layer_count = 0;
  for (const Error* current = this; current != nullptr;
       current = current->unwrap()) {
    const auto* impl = current->get_impl();
    if (impl == nullptr) {
      break;
    }
    total_len += impl->message_view().size();
    ++layer_count;
  }

  if (layer_count > 1) {
    total_len += (layer_count - 1) * 2;  // ": " separators
  }

  std::string result;
  result.reserve(total_len);
  for (const Error* current = this; current != nullptr;
       current = current->unwrap()) {
    const auto* impl = current->get_impl();
    if (impl == nullptr) {
      break;
    }
    auto view = impl->message_view();
    if (!result.empty()) {
      result += ": ";
    }
    result.append(view.data(), view.size());
  }

  return result;
}

auto Error::what() const noexcept -> std::string_view {
  const auto* impl = get_impl();
  return impl != nullptr ? impl->message_view() : std::string_view{};
}

auto Error::Unwrap() const noexcept -> const Error* { return unwrap(); }

auto Error::Nil() noexcept -> const Error& {
  static const Error nil_inst;
  return nil_inst;
}

// NOLINTNEXTLINE(fuchsia-overloaded-operator)
auto operator==(const Error& lhs, const Error& rhs) noexcept -> bool {
  return lhs.bits_ == rhs.bits_;
}

// Factory Functions

auto New(std::string_view msg) -> Error {
  return Error(new internal::DynamicError(msg));
}

auto Wrap(Error inner, std::string_view msg) -> Error {
  return Error(new internal::DynamicError(msg, std::move(inner)));
}

// Introspection
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto Is(const Error& err, const Error& target) -> bool {
  for (const Error* curr = &err; curr != nullptr; curr = curr->unwrap()) {
    if (*curr == target) {
      return true;
    }
    // Extensible matching hook: let implementations define custom equality.
    if (const auto* impl = curr->get_impl()) {
      if (impl->matches(target)) {
        return true;
      }
    }
  }
  return false;
}

// Serialization

auto IsSerializable(const Error& err) -> bool {
  if (!err) {
    return true;
  }

  for (const Error* current = &err; current != nullptr;
       current = current->unwrap()) {
    const auto* impl = current->get_impl();
    if (impl == nullptr || !impl->is_serializable()) {
      return false;
    }
  }
  return true;
}

auto DebugString(const Error& err) -> std::string {
  if (!err) {
    return "(nil)";
  }

  // Heuristic reserve: sum message sizes + 66 bytes per layer
  // (2 for ": " separator + 64 estimated for payload debug info).
  size_t estimated = 0;
  for (const Error* current = &err; current != nullptr;
       current = current->unwrap()) {
    const auto* impl = current->get_impl();
    if (impl == nullptr) {
      break;
    }
    estimated += impl->message_view().size() + 2 + 64;
  }

  std::string result;
  result.reserve(estimated);
  for (const Error* current = &err; current != nullptr;
       current = current->unwrap()) {
    const auto* impl = current->get_impl();
    if (impl == nullptr) {
      break;
    }

    if (!result.empty()) {
      result += ": ";
    }

    result.append(impl->message_view());
    auto debug = impl->payload_debug_string();
    if (!debug.empty()) {
      result += " [";
      auto url = impl->payload_type_url();
      if (!url.empty()) {
        result += url;
        result += ": ";
      }
      result += debug;
      result += "]";
    }
  }
  return result;
}

auto Serialize(const Error& err) -> std::string {
  if (!err) {
    return {};
  }

  std::string out;
  out.resize(4);  // placeholder for layer count
  uint32_t count = 0;

  auto write_u32 = [&](uint32_t val) -> void {
    char buf[4];                // NOLINT
    std::memcpy(buf, &val, 4);  // NOLINT
    out.append(buf, 4);         // NOLINT
  };

  auto write_bytes = [&](std::string_view strv) -> void {
    assert(strv.size() <= UINT32_MAX);
    write_u32(static_cast<uint32_t>(strv.size()));
    out.append(strv);
  };

  for (const Error* current = &err; current != nullptr;
       current = current->unwrap()) {
    const auto* impl = current->get_impl();
    if (impl == nullptr) {
      break;
    }
    write_bytes(impl->message_view());
    write_bytes(impl->payload_type_url());
    write_bytes(impl->serialize_payload());
    ++count;
  }

  std::memcpy(out.data(), &count, 4);
  return out;
}

auto Deserialize(std::string_view data) -> Error {
  if (data.empty()) {
    return {};
  }
  if (data.size() < 4) {
    return {};
  }

  bool valid = true;

  auto read_u32 = [&valid](std::string_view& deserialized_str) -> uint32_t {
    if (deserialized_str.size() < 4) {
      valid = false;
      return 0;
    }
    uint32_t value;
    std::memcpy(&value, deserialized_str.data(), 4);
    deserialized_str.remove_prefix(4);
    return value;
  };

  auto read_str = [&read_u32,
                   &valid](std::string_view& deserialized_str) -> std::string {
    const uint32_t len = read_u32(deserialized_str);
    if (!valid || len > deserialized_str.size()) {
      valid = false;
      return {};
    }
    std::string str(deserialized_str.substr(0, len));
    deserialized_str.remove_prefix(len);
    return str;
  };

  const uint32_t count = read_u32(data);
  if (!valid || count == 0) {
    return {};
  }

  struct Layer {
    std::string msg;
    std::string type_url;
    std::string payload;
  };
  std::vector<Layer> layers;
  // Each layer needs at minimum 3 uint32 length prefixes = 12 bytes.
  // Cap to prevent OOM from malicious input.
  const size_t max_layers = data.size() / 12;
  layers.reserve(std::min(static_cast<size_t>(count), max_layers));

  for (uint32_t i = 0; i < count && valid; ++i) {
    auto msg = read_str(data);
    auto type_url = read_str(data);
    auto payload = read_str(data);
    if (valid) {
      layers.push_back(
          {std::move(msg), std::move(type_url), std::move(payload)});
    }
  }

  if (layers.empty()) {
    return {};
  }

  // Build chain from innermost to outermost.
  Error err;
  for (std::size_t i = layers.size(); i-- > 0;) {
    auto& layer = layers[i];
    if (!layer.type_url.empty()) {
      SerializedPayload serialized_payload{
          .type_url = std::move(layer.type_url),
          .data = std::move(layer.payload)};
      if (err) {
        err = WrapWithPayload(std::move(err), layer.msg,
                              std::move(serialized_payload));
      } else {
        err = NewWithPayload(layer.msg, std::move(serialized_payload));
      }
    } else {
      if (err) {
        err = Error(new internal::DynamicError(layer.msg, std::move(err)));
      } else {
        err = New(layer.msg);
      }
    }
  }
  return err;
}

namespace internal {

// DynamicError Implementation

DynamicError::DynamicError(std::string_view msg) { set_string(msg); }

DynamicError::DynamicError(std::string_view msg, Error inner)
    : inner_(std::move(inner)) {
  set_string(msg);
}

DynamicError::~DynamicError() {
  if (!is_sso()) {
    delete[] data_.heap_buffer_;  // NOLINT
  }
}

auto DynamicError::set_string(std::string_view msg) -> void {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,
  // bugprone-suspicious-stringview-data-usage)
  size_ = msg.length();
  if (is_sso()) {
    std::copy_n(msg.data(), size_, data_.sso_buffer_);
    data_.sso_buffer_[size_] = '\0';
  } else {
    data_.heap_buffer_ = new char[size_ + 1];
    std::copy_n(msg.data(), size_, data_.heap_buffer_);
    data_.heap_buffer_[size_] = '\0';
  }
  // NOLINTEND(cppcoreguidelines-pro-type-union-access,
  // bugprone-suspicious-stringview-data-usage)
}

auto DynamicError::message_view() const noexcept -> std::string_view {
  // NOLINTNEXTLINE(modernize-return-braced-init-list,cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay,cppcoreguidelines-pro-type-union-access)
  return std::string_view(is_sso() ? data_.sso_buffer_ : data_.heap_buffer_,
                          size_);
}

auto DynamicError::unwrap() const noexcept -> const Error* {
  return inner_ ? &inner_ : nullptr;
}

auto DynamicError::clone() const -> ErrorImpl* {
  return new DynamicError(*this);  // NOLINT(cppcoreguidelines-owning-memory)
}

// Manual management for rule of 5
DynamicError::DynamicError(const DynamicError& other) : inner_(other.inner_) {
  set_string(other.message_view());
}

auto DynamicError::operator=(const DynamicError& other) -> DynamicError& {
  if (this != &other) {
    if (!is_sso()) {
      delete[] data_.heap_buffer_;  // NOLINT
    }
    set_string(other.message_view());
    inner_ = other.inner_;
  }
  return *this;
}

DynamicError::DynamicError(DynamicError&& other) noexcept
    : size_(other.size_), inner_(std::move(other.inner_)) {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,
  // bugprone-suspicious-stringview-data-usage)
  if (is_sso()) {
    std::copy_n(other.data_.sso_buffer_, size_ + 1, data_.sso_buffer_);
  } else {
    data_.heap_buffer_ = other.data_.heap_buffer_;
    other.data_.heap_buffer_ = nullptr;
  }

  other.size_ = 0;
  other.data_.sso_buffer_[0] = '\0';
  // NOLINTEND(cppcoreguidelines-pro-type-union-access,
  // bugprone-suspicious-stringview-data-usage)
}

auto DynamicError::operator=(DynamicError&& other) noexcept -> DynamicError& {
  if (this != &other) {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,
    // bugprone-suspicious-stringview-data-usage)
    if (!is_sso()) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      delete[] data_.heap_buffer_;
    }
    size_ = other.size_;
    if (is_sso()) {
      std::copy_n(other.data_.sso_buffer_, size_ + 1, data_.sso_buffer_);
    } else {
      data_.heap_buffer_ = other.data_.heap_buffer_;
      other.data_.heap_buffer_ = nullptr;
    }
    inner_ = std::move(other.inner_);
    other.size_ = 0;
    other.data_.sso_buffer_[0] = '\0';
    // NOLINTEND(cppcoreguidelines-pro-type-union-access,
    // bugprone-suspicious-stringview-data-usage)
  }
  return *this;
}

}  // namespace internal
}  // namespace errors
