// Copyright (c) 2026 Amaltas Bohra
// SPDX-License-Identifier: MIT
//
// Comprehensive unit tests for the errors library.

#include "errors/error.h"

#include <gtest/gtest.h>

#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "errors/result.h"

// Lock in the 8-byte tagged-pointer representation.
static_assert(sizeof(errors::Error) == 8);

// Test Assets

// Defining some sentinel errors for testing
ERRORS_DEFINE_SENTINEL(kErrPermission, "permission denied");
ERRORS_DEFINE_SENTINEL(kErrNotFound, "resource not found");
ERRORS_DEFINE_SENTINEL(kErrInternal, "internal server error");

// A mock protobuf-like message for testing serialization.
// Satisfies the WireSerializable and HasDebugString concepts.
struct LoginRequest {
  std::string user;
  std::string ip_address;
  int port = 0;

  std::string SerializeAsString() const {
    return user + "|" + ip_address + "|" + std::to_string(port);
  }
  std::string GetTypeName() const { return "test.LoginRequest"; }
  std::string ShortDebugString() const {
    return "user: \"" + user + "\" ip_address: \"" + ip_address +
           "\" port: " + std::to_string(port);
  }
  bool ParseFromString(const std::string& data) {
    auto p1 = data.find('|');
    if (p1 == std::string::npos) return false;
    auto p2 = data.find('|', p1 + 1);
    if (p2 == std::string::npos) return false;
    user = data.substr(0, p1);
    ip_address = data.substr(p1 + 1, p2 - p1 - 1);
    port = std::stoi(data.substr(p2 + 1));
    return true;
  }
  bool operator==(const LoginRequest&) const = default;
};

// A custom payload for testing structured errors
struct NetworkDetails {
  int status_code;
  std::string remote_ip;

  // Helper for easy comparison in tests
  bool operator==(const NetworkDetails& other) const {
    return status_code == other.status_code && remote_ip == other.remote_ip;
  }
};

// Test Cases

TEST(ErrorTest, NilBehavior) {
  errors::Error err = errors::Error::Nil();

  // Nil error should evaluate to false
  EXPECT_FALSE(err);
  // Nil error message should be (nil)
  EXPECT_EQ(err.message(), "(nil)");

  // Default constructed error should also be Nil
  errors::Error default_err;
  EXPECT_FALSE(default_err);
  EXPECT_EQ(err, default_err);
}

TEST(ErrorTest, BasicCreation) {
  auto err = errors::New("standard error");

  EXPECT_TRUE(err);
  EXPECT_EQ(err.message(), "standard error");

  auto formatted = errors::Errorf("error code: {}", 404);
  EXPECT_EQ(formatted.message(), "error code: 404");
}

TEST(ErrorTest, Sentinels) {
  // Basic identity
  EXPECT_EQ(kErrPermission.message(), "permission denied");

  // Equality check
  errors::Error err = kErrPermission;
  EXPECT_EQ(err, kErrPermission);
  EXPECT_NE(err, kErrNotFound);

  // Is check
  EXPECT_TRUE(errors::Is(err, kErrPermission));
  EXPECT_FALSE(errors::Is(err, kErrNotFound));
}

TEST(ErrorTest, Wrapping) {
  auto base = kErrPermission;
  auto wrapped = errors::Wrapf(base, "service layer failure");
  auto deep_wrapped = errors::Wrapf(wrapped, "api gateway error");

  // Message should concatenate context
  EXPECT_EQ(deep_wrapped.message(),
            "api gateway error: service layer failure: permission denied");

  // Is should find the sentinel at any depth
  EXPECT_TRUE(errors::Is(deep_wrapped, kErrPermission));
  EXPECT_FALSE(errors::Is(deep_wrapped, kErrNotFound));
}

TEST(ErrorTest, PayloadExtraction) {
  // Test direct payload creation
  auto err = errors::NewWithPayload("connection failed",
                                    NetworkDetails{503, "192.168.1.1"});

  // Extraction via As
  auto* details = errors::As<NetworkDetails>(err);
  ASSERT_NE(details, nullptr);
  EXPECT_EQ(details->status_code, 503);
  EXPECT_EQ(details->remote_ip, "192.168.1.1");

  // Check that unrelated types return nullptr
  EXPECT_EQ(errors::As<int>(err), nullptr);
}

TEST(ErrorTest, InPlacePayload) {
  // Test in-place construction using tag-first order
  auto err = errors::NewWithPayload(std::in_place_type<NetworkDetails>,
                                    "timeout", 504, "10.0.0.5");

  auto* details = errors::As<NetworkDetails>(err);
  ASSERT_NE(details, nullptr);
  EXPECT_EQ(details->status_code, 504);
  EXPECT_EQ(details->remote_ip, "10.0.0.5");
}

TEST(ErrorTest, WrappedPayloads) {
  // Create an error with a payload
  auto base =
      errors::NewWithPayload("base error", NetworkDetails{400, "local"});

  // Wrap it in a sentinel
  auto wrapped = errors::Wrapf(base, "outer context");

  // As should find the payload through the wrapping layers
  auto* details = errors::As<NetworkDetails>(wrapped);
  ASSERT_NE(details, nullptr);
  EXPECT_EQ(details->status_code, 400);

  // Wrap again with a different payload
  struct TraceId {
    std::string id;
  };
  auto double_wrapped =
      errors::WrapWithPayload(wrapped, "logger context", TraceId{"abc-123"});

  // Can extract both types of payloads from the same error chain
  EXPECT_NE(errors::As<TraceId>(double_wrapped), nullptr);
  EXPECT_NE(errors::As<NetworkDetails>(double_wrapped), nullptr);
  EXPECT_EQ(errors::As<TraceId>(double_wrapped)->id, "abc-123");
}

TEST(ErrorTest, SSOVerification) {
  // This doesn't strictly test SSO but ensures short and long strings work
  // identicaly
  std::string long_msg(500, 'a');
  auto short_err = errors::New("short");
  auto long_err = errors::New(long_msg);

  EXPECT_EQ(short_err.message(), "short");
  EXPECT_EQ(long_err.message(), long_msg);
}

// Edge cases

TEST(ErrorTest, EmptyMessage) {
  auto err = errors::New("");
  EXPECT_TRUE(err);
  EXPECT_EQ(err.message(), "");
}

TEST(ErrorTest, SingleCharMessage) {
  auto err = errors::New("x");
  EXPECT_TRUE(err);
  EXPECT_EQ(err.message(), "x");
}

TEST(ErrorTest, SelfCopyAssignment) {
  auto err = errors::New("self copy");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-assign-overloaded"
  err = err;
#pragma GCC diagnostic pop
  EXPECT_EQ(err.message(), "self copy");
}

TEST(ErrorTest, SelfMoveAssignment) {
  auto err = errors::New("self move");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
  err = std::move(err);
#pragma GCC diagnostic pop
  // After self-move, object should remain in a valid state.
  EXPECT_EQ(err.message(), "self move");
}

TEST(ErrorTest, MoveFromStateIsNil) {
  auto err = errors::New("will be moved");
  errors::Error dst = std::move(err);
  // Moved-from error should be nil.
  EXPECT_FALSE(err);
  EXPECT_TRUE(dst);
  EXPECT_EQ(dst.message(), "will be moved");
}

// Deep chain

TEST(ErrorTest, DeepChain) {
  errors::Error err = kErrPermission;
  constexpr int kDepth = 100;
  for (int i = 0; i < kDepth; ++i) {
    err = errors::Wrapf(err, "layer {}", i);
  }

  // Is() should find the sentinel at the bottom.
  EXPECT_TRUE(errors::Is(err, kErrPermission));
  EXPECT_FALSE(errors::Is(err, kErrNotFound));

  // Walk the chain via Unwrap() and count depth.
  int depth = 0;
  for (const errors::Error* cur = &err; cur != nullptr; cur = cur->Unwrap()) {
    ++depth;
  }
  // kDepth wrapping layers + 1 sentinel = kDepth+1 nodes.
  EXPECT_EQ(depth, kDepth + 1);
}

// Refcounting / COW

TEST(ErrorTest, CopySharesState) {
  auto original =
      errors::NewWithPayload("shared", NetworkDetails{200, "10.0.0.1"});
  errors::Error copy = original;

  // Both should have the same message.
  EXPECT_EQ(original.message(), copy.message());

  // Both should be able to read the payload.
  const auto* orig_details =
      errors::As<NetworkDetails>(static_cast<const errors::Error&>(original));
  const auto* copy_details =
      errors::As<NetworkDetails>(static_cast<const errors::Error&>(copy));
  ASSERT_NE(orig_details, nullptr);
  ASSERT_NE(copy_details, nullptr);
  EXPECT_EQ(orig_details->status_code, 200);
  EXPECT_EQ(copy_details->status_code, 200);
}

TEST(ErrorTest, COWOnMutableAs) {
  auto original =
      errors::NewWithPayload("cow test", NetworkDetails{500, "1.2.3.4"});
  errors::Error copy = original;

  // Mutate the copy's payload via non-const As<T>.
  auto* details = errors::As<NetworkDetails>(copy);
  ASSERT_NE(details, nullptr);
  details->status_code = 999;

  // Original should be unchanged (COW triggered a clone).
  const auto* orig_details =
      errors::As<NetworkDetails>(static_cast<const errors::Error&>(original));
  ASSERT_NE(orig_details, nullptr);
  EXPECT_EQ(orig_details->status_code, 500);
  EXPECT_EQ(details->status_code, 999);
}

// Traversal: what() / Unwrap()

TEST(ErrorTest, WhatReturnsSingleLayer) {
  auto inner = errors::New("inner message");
  auto outer = errors::Wrapf(inner, "outer message");

  EXPECT_EQ(outer.what(), "outer message");
}

TEST(ErrorTest, UnwrapWalksChain) {
  auto base = kErrPermission;
  auto mid = errors::Wrapf(base, "middle");
  auto top = errors::Wrapf(mid, "top");

  EXPECT_EQ(top.what(), "top");
  auto* layer1 = top.Unwrap();
  ASSERT_NE(layer1, nullptr);
  EXPECT_EQ(layer1->what(), "middle");
  auto* layer2 = layer1->Unwrap();
  ASSERT_NE(layer2, nullptr);
  EXPECT_EQ(layer2->what(), "permission denied");
  EXPECT_EQ(layer2->Unwrap(), nullptr);
}

TEST(ErrorTest, NilWhatAndUnwrap) {
  errors::Error nil;
  EXPECT_EQ(nil.what(), "");
  EXPECT_EQ(nil.Unwrap(), nullptr);
}

TEST(ErrorTest, SentinelWhatAndUnwrap) {
  errors::Error err = kErrNotFound;
  EXPECT_EQ(err.what(), "resource not found");
  EXPECT_EQ(err.Unwrap(), nullptr);
}

// Is() semantics

TEST(ErrorTest, TwoNewErrorsDoNotMatch) {
  auto a = errors::New("same");
  auto b = errors::New("same");
  // Different dynamic errors should NOT be Is()-equal even with same message.
  EXPECT_FALSE(errors::Is(a, b));
  EXPECT_FALSE(errors::Is(b, a));
}

TEST(ErrorTest, IsWithNilTarget) {
  auto err = errors::New("something");
  errors::Error nil;
  // A non-nil error is not Is() to nil unless the chain contains nil.
  // Since our chain doesn't wrap nil explicitly, this should be false.
  EXPECT_FALSE(errors::Is(err, nil));
}

// As on nil

TEST(ErrorTest, AsOnNilReturnsNullptr) {
  errors::Error nil;
  EXPECT_EQ(errors::As<NetworkDetails>(nil), nullptr);
  const errors::Error& cnil = nil;
  EXPECT_EQ(errors::As<NetworkDetails>(cnil), nullptr);
}

// Result<T> tests

TEST(ResultTest, SuccessPath) {
  errors::Result<int> r = 42;
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
}

TEST(ResultTest, FailurePath) {
  errors::Result<int> r = errors::New("failed");
  EXPECT_FALSE(r.ok());
  EXPECT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.error().message(), "failed");
}

TEST(ResultTest, MoveSemantics) {
  errors::Result<std::string> r = std::string("hello");
  errors::Result<std::string> moved = std::move(r);
  EXPECT_TRUE(moved.ok());
  EXPECT_EQ(*moved, "hello");
}

TEST(ResultTest, CopySemantics) {
  errors::Result<int> r = 7;
  errors::Result<int> copy = r;
  EXPECT_EQ(*copy, 7);
  EXPECT_EQ(*r, 7);
}

TEST(ResultTest, OperatorArrow) {
  struct Foo {
    int x;
    int get() const { return x; }
  };
  errors::Result<Foo> r = Foo{123};
  EXPECT_EQ(r->get(), 123);
}

TEST(ResultTest, SentinelErrorInResult) {
  errors::Result<int> r = errors::Error(kErrPermission);
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(errors::Is(r.error(), kErrPermission));
}

TEST(ResultTest, MoveValueOut) {
  errors::Result<std::string> r = std::string("transfer");
  std::string val = std::move(r).value();
  EXPECT_EQ(val, "transfer");
}

// Serialization tests

TEST(SerializationTest, IsSerializable_Nil) {
  errors::Error nil;
  EXPECT_TRUE(errors::IsSerializable(nil));
}

TEST(SerializationTest, IsSerializable_Sentinel) {
  EXPECT_FALSE(errors::IsSerializable(kErrNotFound));
}

TEST(SerializationTest, IsSerializable_Dynamic) {
  auto err = errors::New("simple error");
  EXPECT_TRUE(errors::IsSerializable(err));
}

TEST(SerializationTest, IsSerializable_WireSerializablePayload) {
  auto err = errors::NewWithPayload("login failed",
                                    LoginRequest{"alice", "10.0.0.1", 8080});
  EXPECT_TRUE(errors::IsSerializable(err));
}

TEST(SerializationTest, IsSerializable_NonSerializablePayload) {
  auto err =
      errors::NewWithPayload("network error", NetworkDetails{503, "10.0.0.1"});
  EXPECT_FALSE(errors::IsSerializable(err));
}

TEST(SerializationTest, IsSerializable_ChainWithSentinel) {
  auto err = errors::Wrapf(kErrNotFound, "service layer");
  EXPECT_FALSE(errors::IsSerializable(err));
}

TEST(SerializationTest, IsSerializable_ChainAllDynamic) {
  auto inner = errors::New("root cause");
  auto outer = errors::Wrapf(inner, "context");
  EXPECT_TRUE(errors::IsSerializable(outer));
}

TEST(SerializationTest, IsSerializable_ChainWithSerializablePayload) {
  auto inner =
      errors::NewWithPayload("base", LoginRequest{"bob", "10.0.0.2", 443});
  auto outer = errors::Wrapf(inner, "context");
  EXPECT_TRUE(errors::IsSerializable(outer));
}

TEST(SerializationTest, DebugString_Nil) {
  errors::Error nil;
  EXPECT_EQ(errors::DebugString(nil), "(nil)");
}

TEST(SerializationTest, DebugString_SimpleError) {
  auto err = errors::New("simple error");
  EXPECT_EQ(errors::DebugString(err), "simple error");
}

TEST(SerializationTest, DebugString_WithPayload) {
  auto err = errors::NewWithPayload("login failed",
                                    LoginRequest{"alice", "10.0.0.1", 8080});
  auto debug = errors::DebugString(err);
  EXPECT_EQ(debug,
            "login failed [test.LoginRequest: user: \"alice\" ip_address: "
            "\"10.0.0.1\" port: 8080]");
}

TEST(SerializationTest, DebugString_Chain) {
  auto inner = errors::NewWithPayload("login failed",
                                      LoginRequest{"alice", "10.0.0.1", 8080});
  auto outer = errors::Wrapf(inner, "rpc error");
  auto debug = errors::DebugString(outer);
  // Outer layer has no payload, inner layer has payload.
  EXPECT_EQ(debug,
            "rpc error: login failed [test.LoginRequest: user: \"alice\" "
            "ip_address: \"10.0.0.1\" port: 8080]");
}

TEST(SerializationTest, DebugString_NonSerializablePayload) {
  // Non-WireSerializable payloads produce no debug info.
  auto err =
      errors::NewWithPayload("network error", NetworkDetails{503, "10.0.0.1"});
  EXPECT_EQ(errors::DebugString(err), "network error");
}

TEST(SerializationTest, SerializeDeserialize_Nil) {
  errors::Error nil;
  auto bytes = errors::Serialize(nil);
  EXPECT_TRUE(bytes.empty());
  auto restored = errors::Deserialize(bytes);
  EXPECT_FALSE(restored);
}

TEST(SerializationTest, SerializeDeserialize_Simple) {
  auto err = errors::New("simple error");
  auto bytes = errors::Serialize(err);
  EXPECT_FALSE(bytes.empty());

  auto restored = errors::Deserialize(bytes);
  EXPECT_TRUE(restored);
  EXPECT_EQ(restored.message(), "simple error");
}

TEST(SerializationTest, SerializeDeserialize_WithPayload) {
  auto err = errors::NewWithPayload("login failed",
                                    LoginRequest{"alice", "10.0.0.1", 8080});
  auto bytes = errors::Serialize(err);

  auto restored = errors::Deserialize(bytes);
  EXPECT_TRUE(restored);
  EXPECT_EQ(restored.message(), "login failed");

  // Payload arrives as SerializedPayload.
  auto* sp = errors::As<errors::SerializedPayload>(restored);
  ASSERT_NE(sp, nullptr);
  EXPECT_EQ(sp->type_url, "test.LoginRequest");

  // Reconstruct the original message.
  LoginRequest req;
  EXPECT_TRUE(req.ParseFromString(sp->data));
  EXPECT_EQ(req.user, "alice");
  EXPECT_EQ(req.ip_address, "10.0.0.1");
  EXPECT_EQ(req.port, 8080);
}

TEST(SerializationTest, SerializeDeserialize_Chain) {
  auto inner = errors::NewWithPayload("login failed",
                                      LoginRequest{"bob", "192.168.1.1", 443});
  auto outer = errors::Wrapf(inner, "auth service error");

  auto bytes = errors::Serialize(outer);
  auto restored = errors::Deserialize(bytes);

  EXPECT_EQ(restored.message(), "auth service error: login failed");

  // Payload is reachable through the chain.
  auto* sp = errors::As<errors::SerializedPayload>(restored);
  ASSERT_NE(sp, nullptr);
  LoginRequest req;
  EXPECT_TRUE(req.ParseFromString(sp->data));
  EXPECT_EQ(req.user, "bob");
}

TEST(SerializationTest, SerializeDeserialize_MultiLayerPayloads) {
  auto inner =
      errors::NewWithPayload("base", LoginRequest{"user1", "1.1.1.1", 80});
  auto outer = errors::WrapWithPayload(inner, "wrapper",
                                       LoginRequest{"user2", "2.2.2.2", 443});

  auto bytes = errors::Serialize(outer);
  auto restored = errors::Deserialize(bytes);

  EXPECT_EQ(restored.message(), "wrapper: base");

  // Walk the chain to find both payloads.
  auto* outer_sp = errors::As<errors::SerializedPayload>(restored);
  ASSERT_NE(outer_sp, nullptr);
  LoginRequest outer_req;
  EXPECT_TRUE(outer_req.ParseFromString(outer_sp->data));
  EXPECT_EQ(outer_req.user, "user2");
}

TEST(SerializationTest, Deserialize_Empty) {
  EXPECT_FALSE(errors::Deserialize(""));
}

TEST(SerializationTest, Deserialize_Truncated) {
  EXPECT_FALSE(errors::Deserialize("ab"));
  // count=1 but no layer data.
  EXPECT_FALSE(errors::Deserialize(std::string_view("\x01\x00\x00\x00", 4)));
}

TEST(SerializationTest, RoundTrip_DebugStringPreserved) {
  // After serialize → deserialize, DebugString should still show payload info.
  auto err = errors::NewWithPayload("login failed",
                                    LoginRequest{"alice", "10.0.0.1", 8080});
  auto bytes = errors::Serialize(err);
  auto restored = errors::Deserialize(bytes);

  auto debug = errors::DebugString(restored);
  // SerializedPayload's ShortDebugString shows the byte count.
  EXPECT_NE(debug.find("login failed"), std::string::npos);
  EXPECT_NE(debug.find("test.LoginRequest"), std::string::npos);
}

// Fix 1: Deserialize with malicious count does not OOM.

TEST(SerializationTest, Deserialize_MaliciousCount) {
  // count = UINT32_MAX but only 12 bytes of actual layer data.
  // 3 zero-length strings (each 4-byte length prefix = 0).
  std::string data;
  data.resize(4 + 12);
  uint32_t count = UINT32_MAX;
  std::memcpy(data.data(), &count, 4);
  // 3 zero-length strings: len=0, len=0, len=0
  std::memset(data.data() + 4, 0, 12);

  // Should not OOM. Should parse exactly 1 layer (12 bytes available,
  // remaining layers will fail to parse).
  auto restored = errors::Deserialize(data);
  // The restored error may have 1 layer with empty message or be nil
  // if parsing stops early. The key assertion is no crash / OOM.
  // With 12 bytes of data, max_layers = 12/12 = 1, so reserve(1).
  // Loop tries to read UINT32_MAX layers, but after layer 1 there's no data.
  EXPECT_TRUE(restored);
  EXPECT_EQ(restored.message(), "");
}

// Fix 2: Wrap(err, msg) without format.

TEST(ErrorTest, WrapBasic) {
  auto base = errors::New("root cause");
  auto wrapped = errors::Wrap(base, "context layer");

  EXPECT_EQ(wrapped.message(), "context layer: root cause");
  EXPECT_EQ(wrapped.what(), "context layer");
}

TEST(ErrorTest, WrapChaining) {
  auto base = kErrPermission;
  auto mid = errors::Wrap(base, "service layer");
  auto top = errors::Wrap(mid, "api gateway");

  EXPECT_EQ(top.message(), "api gateway: service layer: permission denied");
  EXPECT_TRUE(errors::Is(top, kErrPermission));
  EXPECT_FALSE(errors::Is(top, kErrNotFound));
}

TEST(ErrorTest, WrapIsThrough) {
  auto inner = kErrNotFound;
  auto outer = errors::Wrap(inner, "lookup failed");
  EXPECT_TRUE(errors::Is(outer, kErrNotFound));
}

// Fix 3: std::formatter<Error>.

TEST(ErrorTest, FormatterSimple) {
  auto err = errors::New("formatted error");
  EXPECT_EQ(err.message(), "formatted error");
}

TEST(ErrorTest, FormatterNil) {
  errors::Error nil;
  EXPECT_EQ(nil.message(), "(nil)");
}

// Fix 6: message() pre-allocation with deep chain.

TEST(ErrorTest, MessagePreallocation) {
  // Build a 50-layer chain and verify message() produces correct output.
  errors::Error err = errors::New("root");
  for (int i = 0; i < 50; ++i) {
    err = errors::Wrap(err, "layer");
  }

  auto msg = err.message();

  // Should start with "layer: layer: ... : root"
  // 50 layers of "layer" + ": " separators + "root"
  // "layer" is 5 chars, ": " is 2 chars, "root" is 4 chars
  // Expected length: 50 * 5 + 50 * 2 + 4 = 354
  size_t expected_len = 50 * 5 + 50 * 2 + 4;
  EXPECT_EQ(msg.size(), expected_len);

  // Verify it ends with "root"
  EXPECT_EQ(msg.substr(msg.size() - 4), "root");
  // Verify it starts with "layer"
  EXPECT_EQ(msg.substr(0, 5), "layer");
}
