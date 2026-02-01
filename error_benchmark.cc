// Copyright (c) 2026 Amaltas Bohra
// SPDX-License-Identifier: MIT
//
// Comprehensive benchmarks for the errors library, measuring core operations
// and comparing against standard C++ error handling alternatives.

#include <benchmark/benchmark.h>

#include <expected>
#include <stdexcept>
#include <string>
#include <system_error>

#include "errors/error.h"
#include "errors/result.h"

namespace {

//------------------------------------------------------------------------
// Test fixtures and helpers
//------------------------------------------------------------------------

ERRORS_DEFINE_SENTINEL(kBenchSentinel, "benchmark sentinel error");
ERRORS_DEFINE_SENTINEL(kBenchTarget, "target sentinel");
ERRORS_DEFINE_SENTINEL(kBenchOther, "other sentinel");

struct BenchPayload {
  int code;
  std::string detail;
};

// Build an error chain of the given depth, with kBenchTarget at the root.
auto MakeChain(std::size_t depth) -> errors::Error {
  errors::Error err = kBenchTarget;
  for (std::size_t i = 0; i < depth; ++i) {
    err = errors::Wrapf(err, "layer {}", i);
  }
  return err;
}

// Build an error chain with a BenchPayload at the root.
auto MakePayloadChain(std::size_t depth) -> errors::Error {
  errors::Error err = errors::NewWithPayload(
      "payload base", BenchPayload{.code = 42, .detail = "bench"});
  for (std::size_t i = 0; i < depth; ++i) {
    err = errors::Wrapf(err, "layer {}", i);
  }
  return err;
}

// 50-character string that exceeds SSO capacity (23 bytes).
constexpr std::string_view kHeapMsg =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";  // 50 x's

//------------------------------------------------------------------------
// A. Creation benchmarks
//------------------------------------------------------------------------

void BM_NilCreation(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Error err;
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_NilCreation);

void BM_SentinelCopy(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Error err = kBenchSentinel;
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_SentinelCopy);

void BM_New_SSO(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::New("short err");
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_New_SSO);

void BM_New_Heap(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::New(kHeapMsg);
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_New_Heap);

void BM_Errorf_SSO(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::Errorf("code: {}", 42);
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Errorf_SSO);

void BM_Errorf_Heap(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err =
        errors::Errorf("this is a long formatted error message: {}", kHeapMsg);
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Errorf_Heap);

void BM_NewWithPayload(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::NewWithPayload(
        "msg", BenchPayload{.code = 42, .detail = "bench"});
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_NewWithPayload);

//------------------------------------------------------------------------
// B. Wrapping benchmarks
//------------------------------------------------------------------------

void BM_Wrapf_SSO(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::Wrapf(kBenchSentinel, "context");
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Wrapf_SSO);

void BM_Wrapf_Heap(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::Wrapf(kBenchSentinel, "{}", kHeapMsg);
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Wrapf_Heap);

void BM_WrapChain(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {  // NOLINT
    auto err = MakeChain(depth);
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_WrapChain)->Arg(1)->Arg(5)->Arg(10);

//------------------------------------------------------------------------
// C. Inspection benchmarks
//------------------------------------------------------------------------

void BM_BoolCheck(benchmark::State& state) {
  const auto err = errors::New("check");
  for (auto _ : state) {  // NOLINT
    bool result = static_cast<bool>(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_BoolCheck);

void BM_BoolCheck_Nil(benchmark::State& state) {
  const errors::Error err;
  for (auto _ : state) {  // NOLINT
    bool result = static_cast<bool>(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_BoolCheck_Nil);

void BM_Equality_Match(benchmark::State& state) {
  const errors::Error a_err = kBenchSentinel;
  const errors::Error b_err = kBenchSentinel;
  for (auto _ : state) {  // NOLINT
    bool result = (a_err == b_err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Equality_Match);

void BM_Equality_Mismatch(benchmark::State& state) {
  const errors::Error a_err = kBenchSentinel;
  const errors::Error b_err = kBenchOther;
  for (auto _ : state) {  // NOLINT
    bool result = (a_err == b_err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Equality_Mismatch);

void BM_Message_SSO(benchmark::State& state) {
  const auto err = errors::New("short err");
  for (auto _ : state) {  // NOLINT
    auto msg = err.message();
    benchmark::DoNotOptimize(msg);
  }
}
BENCHMARK(BM_Message_SSO);

void BM_Message_Chain(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    auto msg = err.message();
    benchmark::DoNotOptimize(msg);
  }
}
BENCHMARK(BM_Message_Chain)->Arg(1)->Arg(5)->Arg(10);

//------------------------------------------------------------------------
// D. Chain Traversal benchmarks
//------------------------------------------------------------------------

void BM_Is_Found_Depth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    bool result = errors::Is(err, kBenchTarget);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Is_Found_Depth)->Arg(1)->Arg(5)->Arg(10);

void BM_Is_NotFound_Depth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    bool result = errors::Is(err, kBenchOther);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Is_NotFound_Depth)->Arg(1)->Arg(5)->Arg(10);

void BM_As_Found_Depth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakePayloadChain(depth);
  for (auto _ : state) {  // NOLINT
    auto* result = errors::As<BenchPayload>(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_As_Found_Depth)->Arg(1)->Arg(5)->Arg(10);

void BM_As_NotFound_Depth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    auto* result = errors::As<BenchPayload>(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_As_NotFound_Depth)->Arg(1)->Arg(5)->Arg(10);

//------------------------------------------------------------------------
// E. Copy / Move benchmarks
//------------------------------------------------------------------------

void BM_Copy_Sentinel(benchmark::State& state) {
  const errors::Error src = kBenchSentinel;
  for (auto _ : state) {  // NOLINT
    errors::Error copy = src;
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(BM_Copy_Sentinel);

void BM_Copy_SSO(benchmark::State& state) {
  auto src = errors::New("short err");
  for (auto _ : state) {  // NOLINT
    errors::Error copy = src;
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(BM_Copy_SSO);

void BM_Copy_Heap(benchmark::State& state) {
  auto src = errors::New(kHeapMsg);
  for (auto _ : state) {  // NOLINT
    errors::Error copy = src;
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(BM_Copy_Heap);

void BM_Copy_Chain(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  auto src = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    errors::Error copy = src;
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(BM_Copy_Chain)->Arg(1)->Arg(5)->Arg(10)->Arg(50);

// noinline wrapper forces the copy to actually happen (prevents
// allocation elision across the call boundary).
[[gnu::noinline]] auto CopyError(const errors::Error& src) -> errors::Error {
  return src;
}

void BM_Copy_Chain_Noinline(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  auto src = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    auto copy = CopyError(src);
    benchmark::DoNotOptimize(copy);
  }
}
BENCHMARK(BM_Copy_Chain_Noinline)->Arg(1)->Arg(5)->Arg(10)->Arg(50);

void BM_Move(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    state.PauseTiming();
    auto src = errors::New("move me");
    state.ResumeTiming();
    errors::Error dst = std::move(src);
    benchmark::DoNotOptimize(dst);
  }
}
BENCHMARK(BM_Move);

//------------------------------------------------------------------------
// F. Copy-on-write benchmarks
//------------------------------------------------------------------------

// Measure the cost of copying a shared error and then triggering COW
// via non-const As<T>(). This is the realistic cost users pay when they
// copy a payload error and then mutate it.

void BM_COW_ReadOnly(benchmark::State& state) {
  // Copy + const As: no clone triggered, just refcount bump + read.
  auto src =
      errors::NewWithPayload("cow", BenchPayload{.code = 1, .detail = "read"});
  for (auto _ : state) {             // NOLINT
    const errors::Error copy = src;  // NOLINT
    const auto* payload = errors::As<BenchPayload>(copy);
    benchmark::DoNotOptimize(payload);
  }
}
BENCHMARK(BM_COW_ReadOnly);

void BM_COW_Mutate(benchmark::State& state) {
  // Copy + non-const As: triggers COW clone on first mutable access.
  auto src =
      errors::NewWithPayload("cow", BenchPayload{.code = 1, .detail = "write"});
  for (auto _ : state) {             // NOLINT
    const errors::Error copy = src;  // NOLINT
    const auto* payload = errors::As<BenchPayload>(copy);
    benchmark::DoNotOptimize(payload);
  }
}
BENCHMARK(BM_COW_Mutate);

//------------------------------------------------------------------------
// G. what() / Unwrap() benchmarks
//------------------------------------------------------------------------

void BM_What_SingleLayer(benchmark::State& state) {
  const auto err = errors::New("single layer message");
  for (auto _ : state) {  // NOLINT
    auto msg = err.what();
    benchmark::DoNotOptimize(msg);
  }
}
BENCHMARK(BM_What_SingleLayer);

void BM_What_Sentinel(benchmark::State& state) {
  const errors::Error err = kBenchSentinel;
  for (auto _ : state) {  // NOLINT
    auto msg = err.what();
    benchmark::DoNotOptimize(msg);
  }
}
BENCHMARK(BM_What_Sentinel);

void BM_Unwrap_Depth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    int count = 0;
    for (const errors::Error* cur = &err; cur != nullptr; cur = cur->Unwrap()) {
      ++count;
    }
    benchmark::DoNotOptimize(count);
  }
}
BENCHMARK(BM_Unwrap_Depth)->Arg(1)->Arg(5)->Arg(10);

//------------------------------------------------------------------------
// H. Result<T> benchmarks
//------------------------------------------------------------------------

void BM_Result_SuccessConstruct(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Result<int> res(42);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_Result_SuccessConstruct);

void BM_Result_FailureConstruct(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Result<int> res{errors::New("failure")};
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_Result_FailureConstruct);

void BM_Result_SuccessAccess(benchmark::State& state) {
  errors::Result<int> res(42);
  for (auto _ : state) {  // NOLINT
    int result = *res;
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Result_SuccessAccess);

void BM_Result_OkCheck(benchmark::State& state) {
  const errors::Result<int> res(42);
  for (auto _ : state) {  // NOLINT
    bool res_ok = res.ok();
    benchmark::DoNotOptimize(res_ok);
  }
}
BENCHMARK(BM_Result_OkCheck);

void BM_Result_FailureSentinel(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Result<int> res{errors::Error(kBenchSentinel)};
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_Result_FailureSentinel);

// H.1: Result<T> vs std::expected<T, E>

void BM_Expected_SuccessConstruct(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    std::expected<int, std::string> res = 42;
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_Expected_SuccessConstruct);

void BM_Expected_FailureConstruct(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    std::expected<int, std::string> res =
        std::unexpected(std::string("failure"));
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_Expected_FailureConstruct);

void BM_Expected_SuccessAccess(benchmark::State& state) {
  std::expected<int, std::string> res = 42;
  for (auto _ : state) {  // NOLINT
    int result = *res;
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Expected_SuccessAccess);

void BM_Expected_HasValue(benchmark::State& state) {
  const std::expected<int, std::string> res = 42;
  for (auto _ : state) {  // NOLINT
    bool res_ok = res.has_value();
    benchmark::DoNotOptimize(res_ok);
  }
}
BENCHMARK(BM_Expected_HasValue);

//------------------------------------------------------------------------
// I. Comparative Baselines
//------------------------------------------------------------------------

// noinline helpers to prevent constant-folding

[[gnu::noinline]] auto BaselineErrorsNew() -> errors::Error {
  return errors::New("baseline error");
}

[[gnu::noinline]] auto BaselineSentinel() -> errors::Error {
  return kBenchSentinel;
}

[[gnu::noinline]] auto BaselineExpected() -> std::expected<int, std::string> {
  return std::unexpected(std::string("baseline error"));
}

[[gnu::noinline]] auto BaselineException() -> int {
  throw std::runtime_error("baseline error");
}

[[gnu::noinline]] auto BaselineErrorCode() -> std::error_code {
  return std::make_error_code(std::errc::io_error);
}

[[gnu::noinline]] auto BaselineRawInt() -> int { return -1; }

// F.1: Error creation + return

void BM_Baseline_ErrorsNew(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = BaselineErrorsNew();
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Baseline_ErrorsNew);

void BM_Baseline_Sentinel(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = BaselineSentinel();
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Baseline_Sentinel);

void BM_Baseline_Expected(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto result = BaselineExpected();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Baseline_Expected);

void BM_Baseline_Exception(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    try {
      auto result = BaselineException();
      benchmark::DoNotOptimize(result);
    } catch (const std::runtime_error& e) {
      const auto* msg = e.what();
      benchmark::DoNotOptimize(msg);
    }
  }
}
BENCHMARK(BM_Baseline_Exception);

void BM_Baseline_ErrorCode(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err_code = BaselineErrorCode();
    benchmark::DoNotOptimize(err_code);
  }
}
BENCHMARK(BM_Baseline_ErrorCode);

void BM_Baseline_RawInt(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto result = BaselineRawInt();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Baseline_RawInt);

// F.2: Error identity/type check

void BM_Check_ErrorsIs(benchmark::State& state) {
  const errors::Error err = kBenchSentinel;
  for (auto _ : state) {  // NOLINT
    bool result = errors::Is(err, kBenchSentinel);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Check_ErrorsIs);

void BM_Check_ExpectedHasValue(benchmark::State& state) {
  const std::expected<int, std::string> exp =
      std::unexpected(std::string("error"));
  for (auto _ : state) {  // NOLINT
    bool result = exp.has_value();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Check_ExpectedHasValue);

void BM_Check_ExpectedStringCompare(benchmark::State& state) {
  std::expected<int, std::string> exp = std::unexpected(std::string("target"));
  for (auto _ : state) {  // NOLINT
    bool result = !exp.has_value() && exp.error() == "target";
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Check_ExpectedStringCompare);

void BM_Check_ErrorCodeCompare(benchmark::State& state) {
  auto err_code = std::make_error_code(std::errc::io_error);
  auto target = std::make_error_code(std::errc::io_error);
  for (auto _ : state) {  // NOLINT
    bool result = (err_code == target);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Check_ErrorCodeCompare);

void BM_Check_RawIntCompare(benchmark::State& state) {
  const int err = -1;
  for (auto _ : state) {  // NOLINT
    bool result = (err == -1);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Check_RawIntCompare);

void BM_Check_ExceptionCatch(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    try {
      throw std::runtime_error("type check");
    } catch (const std::runtime_error& e) {
      const auto* msg = e.what();
      benchmark::DoNotOptimize(msg);
    }
  }
}
BENCHMARK(BM_Check_ExceptionCatch);

// F.3: Success path (no error)

void BM_SuccessPath_ErrorsBool(benchmark::State& state) {
  const errors::Error err;
  for (auto _ : state) {  // NOLINT
    bool result = static_cast<bool>(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_SuccessPath_ErrorsBool);

void BM_SuccessPath_ExpectedHasValue(benchmark::State& state) {
  const std::expected<int, std::string> exp{42};
  for (auto _ : state) {  // NOLINT
    bool result = exp.has_value();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_SuccessPath_ExpectedHasValue);

void BM_SuccessPath_ErrorCodeBool(benchmark::State& state) {
  const std::error_code err_code;
  for (auto _ : state) {  // NOLINT
    bool result = static_cast<bool>(err_code);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_SuccessPath_ErrorCodeBool);

void BM_SuccessPath_RawIntCheck(benchmark::State& state) {
  const int err = 0;
  for (auto _ : state) {  // NOLINT
    bool result = (err == 0);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_SuccessPath_RawIntCheck);

// F.4: Error propagation/wrapping

// noinline propagation helpers

[[gnu::noinline]] auto PropagateWrapf(errors::Error inner) -> errors::Error {
  return errors::Wrapf(std::move(inner), "context");
}

[[gnu::noinline]] auto PropagateExpected(std::expected<int, std::string> inner)
    -> std::expected<int, std::string> {
  if (!inner.has_value()) {
    return std::unexpected("context: " + inner.error());
  }
  return inner;
}

[[gnu::noinline]] void PropagateException() {
  try {
    throw;
  } catch (const std::runtime_error& e) {
    throw std::runtime_error(std::string("context: ") + e.what());
  }
}

void BM_Propagate_Wrapf(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {  // NOLINT
    errors::Error err = kBenchSentinel;
    for (std::size_t i = 0; i < depth; ++i) {
      err = PropagateWrapf(std::move(err));
    }
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Propagate_Wrapf)->Arg(1)->Arg(5)->Arg(10);

void BM_Propagate_ExpectedStringConcat(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {  // NOLINT
    std::expected<int, std::string> exp = std::unexpected(std::string("error"));
    for (std::size_t i = 0; i < depth; ++i) {
      exp = PropagateExpected(std::move(exp));
    }
    benchmark::DoNotOptimize(exp);
  }
}
BENCHMARK(BM_Propagate_ExpectedStringConcat)->Arg(1)->Arg(5)->Arg(10);

void BM_Propagate_ExceptionRethrow(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {  // NOLINT
    try {
      try {
        throw std::runtime_error("error");
      } catch (...) {
        for (std::size_t i = 0; i < depth; ++i) {
          PropagateException();
        }
      }
    } catch (const std::runtime_error& e) {
      const auto* msg = e.what();
      benchmark::DoNotOptimize(msg);
    }
  }
}
BENCHMARK(BM_Propagate_ExceptionRethrow)->Arg(1)->Arg(5)->Arg(10);

//------------------------------------------------------------------------
// J. Serialization benchmarks
//------------------------------------------------------------------------

struct BenchProto {
  int value;  // NOLINT(misc-non-private-member-variables-in-classes)
  [[nodiscard]] auto SerializeAsString() const -> std::string {
    return std::to_string(value);
  }

  [[nodiscard]] static auto GetTypeName() -> std::string {
    return "bench.Proto";
  }

  [[nodiscard]] auto ShortDebugString() const -> std::string {
    return "value: " + std::to_string(value);
  }
};

auto MakeSerializableChain(std::size_t depth) -> errors::Error {
  errors::Error err = errors::NewWithPayload("base", BenchProto{42});
  for (std::size_t i = 0; i < depth; ++i) {
    err = errors::Wrapf(err, "layer {}", i);
  }
  return err;
}

// J.1: IsSerializable
void BM_IsSerializable_Simple(benchmark::State& state) {
  const auto err = errors::New("simple");
  for (auto _ : state) {  // NOLINT
    bool result = errors::IsSerializable(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_IsSerializable_Simple);

void BM_IsSerializable_Chain(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeSerializableChain(depth);
  for (auto _ : state) {  // NOLINT
    bool result = errors::IsSerializable(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_IsSerializable_Chain)->Arg(1)->Arg(5)->Arg(10);

void BM_IsSerializable_NotSerializable(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));

  // chain ends with sentinel
  const auto err = MakeChain(depth);
  for (auto _ : state) {  // NOLINT
    bool result = errors::IsSerializable(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_IsSerializable_NotSerializable)->Arg(1)->Arg(5)->Arg(10);

// J.2: DebugString
void BM_DebugString_Simple(benchmark::State& state) {
  const auto err = errors::New("simple error");
  for (auto _ : state) {  // NOLINT
    auto result = errors::DebugString(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_DebugString_Simple);

void BM_DebugString_WithPayload(benchmark::State& state) {
  const auto err = errors::NewWithPayload("error", BenchProto{42});
  for (auto _ : state) {  // NOLINT
    auto result = errors::DebugString(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_DebugString_WithPayload);

void BM_DebugString_Chain(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeSerializableChain(depth);
  for (auto _ : state) {  // NOLINT
    auto result = errors::DebugString(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_DebugString_Chain)->Arg(1)->Arg(5)->Arg(10);

// J.3: Serialize / Deserialize

void BM_Serialize(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeSerializableChain(depth);
  for (auto _ : state) {  // NOLINT
    auto result = errors::Serialize(err);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Serialize)->Arg(1)->Arg(5)->Arg(10);

void BM_Deserialize(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeSerializableChain(depth);
  auto bytes = errors::Serialize(err);
  for (auto _ : state) {  // NOLINT
    auto result = errors::Deserialize(bytes);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Deserialize)->Arg(1)->Arg(5)->Arg(10);

void BM_SerializeRoundtrip(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  const auto err = MakeSerializableChain(depth);
  for (auto _ : state) {  // NOLINT
    auto bytes = errors::Serialize(err);
    auto restored = errors::Deserialize(bytes);
    benchmark::DoNotOptimize(restored);
  }
}
BENCHMARK(BM_SerializeRoundtrip)->Arg(1)->Arg(5)->Arg(10);

//------------------------------------------------------------------------
// K. Wrap (non-template) benchmarks
//------------------------------------------------------------------------

void BM_Wrap_SSO(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::Wrap(kBenchSentinel, "context");
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Wrap_SSO);

void BM_Wrap_Heap(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = errors::Wrap(kBenchSentinel, kHeapMsg);
    benchmark::DoNotOptimize(err);  // NOLINT
  }
}
BENCHMARK(BM_Wrap_Heap);

//------------------------------------------------------------------------
// L. Result<void> benchmarks
//------------------------------------------------------------------------

void BM_Result_VoidSuccess(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Result<void> r;
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Result_VoidSuccess);

void BM_Result_VoidFailure(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    errors::Result<void> r{errors::New("failure")};
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Result_VoidFailure);

//------------------------------------------------------------------------
// M. Macro benchmarks
//------------------------------------------------------------------------

[[gnu::noinline]] auto BenchSucceedingError() -> errors::Error {
  return errors::Error();
}

[[gnu::noinline]] auto BenchFailingError() -> errors::Error {
  return errors::New("macro error");
}

[[gnu::noinline]] auto BenchSucceedingResult() -> errors::Result<int> {
  return 42;
}

[[gnu::noinline]] auto BenchFailingResult() -> errors::Result<int> {
  return errors::New("result error");
}

// M.1: ERRORS_RETURN_IF_ERROR

[[gnu::noinline]] auto MacroReturnIfError_Success() -> errors::Error {
  ERRORS_RETURN_IF_ERROR(BenchSucceedingError());
  return errors::Error();
}

[[gnu::noinline]] auto MacroReturnIfError_Failure() -> errors::Error {
  ERRORS_RETURN_IF_ERROR(BenchFailingError());
  return errors::Error();
}

void BM_Macro_ReturnIfError_Success(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = MacroReturnIfError_Success();
    benchmark::DoNotOptimize(err);
  }
}
BENCHMARK(BM_Macro_ReturnIfError_Success);

void BM_Macro_ReturnIfError_Failure(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto err = MacroReturnIfError_Failure();
    benchmark::DoNotOptimize(err);
  }
}
BENCHMARK(BM_Macro_ReturnIfError_Failure);

// M.2: ERRORS_ASSIGN_OR_RETURN

[[gnu::noinline]] auto MacroAssignOrReturn_Success() -> errors::Result<int> {
  ERRORS_ASSIGN_OR_RETURN(auto val, BenchSucceedingResult());
  return val;
}

[[gnu::noinline]] auto MacroAssignOrReturn_Failure() -> errors::Result<int> {
  ERRORS_ASSIGN_OR_RETURN(auto val, BenchFailingResult());
  return val;
}

void BM_Macro_AssignOrReturn_Success(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto r = MacroAssignOrReturn_Success();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Macro_AssignOrReturn_Success);

void BM_Macro_AssignOrReturn_Failure(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto r = MacroAssignOrReturn_Failure();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Macro_AssignOrReturn_Failure);

// M.3: ERRORS_TRY (GCC/Clang only)

#if defined(__GNUC__)

[[gnu::noinline]] auto MacroTry_Success() -> errors::Result<int> {
  int val = ERRORS_TRY(BenchSucceedingResult());
  return val;
}

[[gnu::noinline]] auto MacroTry_Failure() -> errors::Result<int> {
  int val = ERRORS_TRY(BenchFailingResult());
  return val;
}

void BM_Macro_Try_Success(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto r = MacroTry_Success();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Macro_Try_Success);

void BM_Macro_Try_Failure(benchmark::State& state) {
  for (auto _ : state) {  // NOLINT
    auto r = MacroTry_Failure();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Macro_Try_Failure);

#endif  // defined(__GNUC__)

}  // namespace

// Run the benchmark
BENCHMARK_MAIN();  // NOLINT
