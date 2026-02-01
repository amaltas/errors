# errors

A lightweight, high-performance error handling library for C++23 with
first-class support for error chaining, identity preservation, and
structured payloads.

## Table of contents

- [Why this library?](#why-this-library)
- [Quick start](#quick-start)
- [Usage](#usage)
  - [Creating errors](#creating-errors)
  - [Checking for errors](#checking-for-errors)
  - [Sentinel errors](#sentinel-errors)
  - [Error chaining](#error-chaining----the-core-idea)
  - [Matching errors with `Is`](#matching-errors-with-is)
  - [Structured payloads with `As`](#structured-payloads-with-as)
  - [Wrapping with payloads](#wrapping-with-payloads)
  - [Serializable payloads](#serializable-payloads)
  - [Inspecting individual layers](#inspecting-individual-layers)
  - [`std::format` support](#stdformat-support)
  - [`Result<T>`](#resultt)
  - [`Result<void>`](#resultvoid)
  - [Error propagation macros](#error-propagation-macros)
- [API reference](#api-reference)
- [Performance](#performance)
  - [Characteristics](#performance-characteristics)
  - [Benchmarks](#benchmarks)
- [License](#license)

## Why this library?

C++ has no standard answer to the question "how should a function report
failure?" The language offers exceptions, `std::error_code`,
`std::expected<T,E>`, output parameters, and raw integer return codes.
Each carries trade-offs, but they all share one critical limitation: **errors
lose context as they propagate up the call stack.**

Consider a database connection failure. The raw error is
`"connection refused"`. By the time it reaches an HTTP handler, the caller
has no idea *what* was being connected to, *why*, or *which request* triggered
it. In C++ today you either:

- Catch an exception, construct a *new* exception with more context, and
  re-throw -- losing the original type and stack trace.
- Return `std::expected<T,E>` -- but the error type `E` is fixed at the
  lowest layer and carries no context from intermediate callers. There is no
  mechanism to wrap it.
- Return `absl::Status` -- which supports a message and a canonical code but
  has no built-in chaining. You can concatenate strings manually, but the
  original error identity is lost; `absl::IsNotFound(status)` stops working
  the moment you overwrite the message.
- Return an `std::error_code` -- which has no wrapping, no context strings,
  and no way to carry structured data.

| Mechanism | Uniform type | Chaining | Preserves identity | Structured data | Serializable |
|---|---|---|---|---|---|
| Exceptions | No | No (re-throw loses type) | No | Via subclassing | No |
| `std::expected<T,E>` | No (E varies) | No | N/A | Only what E holds | No |
| `absl::Status` | Yes | No | Lost on rewrap | Payloads (cord-based) | No |
| `std::error_code` | Yes | No | Yes (code survives) | No | No |
| **`errors::Error`** | **Yes** | **Yes** | **Yes** | **Yes** | **Yes** |

This library takes a different approach -- a single, uniform error type with
built-in chaining:

- **One type**: every function that can fail returns `errors::Error`. No
  templates in signatures, no variant gymnastics.
- **Error chaining**: each call site wraps the error with its own context.
  The original cause is preserved, and the final message tells the full story
  from the top of the stack to the root cause.
- **Value semantics**: `Error` is 8 bytes. Copy it, move it, return it.
  No heap allocation for sentinel errors, no exceptions. Copies share state
  via atomic reference counting; mutation triggers copy-on-write.
- **Sentinel identity**: define named error constants (`ErrNotFound`,
  `ErrTimeout`, ...) and match on them through any depth of wrapping.
  `Is` walks the full chain -- wrapping never destroys identity.
- **Structured payloads**: attach arbitrary typed data (HTTP status codes,
  trace IDs, retry metadata) and extract it later with `As<T>`, through
  any depth of wrapping.
- **Serializable payloads**: when the payload is a protobuf message (or any
  type with `SerializeAsString()` and `GetTypeName()`), the entire error --
  message chain, payloads, and all -- can be serialized for logging, storage,
  or wire transport. `DebugString` produces a human-readable dump of the full
  error including payload contents, without the caller needing to know the
  payload type. No protobuf dependency in the core library.

## Quick start

### Requirements

- C++23 compiler (tested with Clang 20, GCC 14)
- CMake 3.14+
- Works with `-fno-rtti` (no RTTI required)

### Integration

Add the `errors/` directory to your project and link against the `errors`
target:

```cmake
add_subdirectory(errors)
target_link_libraries(your_target PRIVATE errors)
```

### Include

```cpp
#include "errors/error.h"    // Error, New, Wrapf, Is, As, ...
#include "errors/result.h"   // Result<T> (header-only, optional)
```

## Usage

### Creating errors

```cpp
// Simple error with a message.
errors::Error err = errors::New("connection refused");

// Formatted error (uses std::format).
errors::Error err = errors::Errorf("port {} is already in use", 8080);
```

### Checking for errors

```cpp
errors::Error DoWork();

auto err = DoWork();
if (err) {
    // handle error
    std::cerr << err.message() << std::endl;
}
```

A nil (no-error) `Error` evaluates to `false`. Any real error evaluates to
`true`. This mirrors Go's `if err != nil` pattern.

### Sentinel errors

Sentinel errors are compile-time constants that represent well-known error
conditions. Define them at namespace scope:

```cpp
// In a header:
ERRORS_DEFINE_SENTINEL(kErrNotFound,   "resource not found");
ERRORS_DEFINE_SENTINEL(kErrPermission, "permission denied");
ERRORS_DEFINE_SENTINEL(kErrTimeout,    "request timed out");
```

Return them directly:

```cpp
errors::Error FindUser(int64_t id) {
    if (!db_.Contains(id))
        return kErrNotFound;
    // ...
    return {};  // nil -- success
}
```

### Error chaining -- the core idea

In a real system, an error crosses many layers before it reaches the caller
that handles it. Each layer knows something the others don't. Error chaining
lets every layer add its own context *without destroying the original cause*.

Consider a request that fails because a database connection is refused:

```cpp
// --- Layer 1: Database driver ---
ERRORS_DEFINE_SENTINEL(kErrConnRefused, "connection refused");

errors::Error Connect(std::string_view host) {
    if (!TcpConnect(host))
        return kErrConnRefused;                    // root cause
    // ...
    return {};
}

// --- Layer 2: Repository ---
errors::Error UserRepo::FindById(int64_t id) {
    auto err = db_.Connect("db-primary:5432");
    if (err)
        return errors::Wrapf(err, "user repo: query user {}", id);
                                                   // adds repo context
    // ...
    return {};
}

// --- Layer 3: Service ---
errors::Error ProfileService::Load(int64_t user_id) {
    auto err = repo_.FindById(user_id);
    if (err)
        return errors::Wrapf(err, "loading profile");
                                                   // adds service context
    // ...
    return {};
}

// --- Layer 4: HTTP handler ---
void HandleGetProfile(int64_t user_id) {
    auto err = service_.Load(user_id);
    if (err) {
        // The full chain is preserved:
        log::Error(err.message());
        // => "loading profile: user repo: query user 42: connection refused"

        // The original sentinel is still reachable through all layers:
        if (errors::Is(err, kErrConnRefused)) {
            Respond(503, "database unavailable");
        }
    }
}
```

The call to `err.message()` produces:

```
loading profile: user repo: query user 42: connection refused
```

Every layer's context is present. An engineer reading this log line can trace
the failure from the HTTP handler, through the service and repository, down to
the exact root cause -- without ever opening a debugger.

**This is the key difference from traditional C++ error handling.** With
`std::error_code` or `absl::Status`, you get the *last* message or the *root*
code, but not both. With exceptions, re-throwing with a new message loses the
original exception type. With `errors::Error`, every layer adds context and
the original identity is preserved.

### Why chaining matters in practice

Without chaining, error messages at the top of the stack are either:

- **Too vague:** `"operation failed"` -- which operation? why?
- **Too specific:** `"connection refused"` -- but *where* in the codebase? what
  was being connected? on behalf of which request?

With chaining, the error message is a breadcrumb trail. Each call site adds
exactly what it knows:

```
api gateway: authenticate user 7: token service: refresh expired token: redis: connection refused
```

No single layer could have produced this full message. It requires cooperation
across the entire call stack, and that cooperation is a one-line `Wrapf` call.

When no formatting is needed, use `Wrap` instead of `Wrapf` to avoid the
overhead of `std::format`:

```cpp
return errors::Wrap(err, "service layer failure");
```

### Matching errors with `Is`

Wrapping never destroys identity. `Is` walks the entire error chain looking
for a match, no matter how many layers of wrapping have been applied:

```cpp
auto err = service_.Load(user_id);

// Works even though the error has been wrapped 3 times:
if (errors::Is(err, kErrConnRefused)) {
    // handle connection failure -- retry, failover, 503, etc.
} else if (errors::Is(err, kErrNotFound)) {
    // handle missing resource -- 404
} else if (err) {
    // handle unknown error -- 500
}
```

This is the C++ equivalent of Go's `errors.Is(err, target)`. The sentinel's
identity survives any depth of wrapping because `Is` compares the tagged
pointer at each node in the chain -- a single integer comparison per hop.

### Structured payloads with `As`

Attach arbitrary typed data to an error and extract it later:

```cpp
struct HttpError {
    int status_code;
    std::string url;
};

// Create an error with a payload.
auto err = errors::NewWithPayload(
    "request failed", HttpError{503, "https://api.example.com/v1/users"});

// Later, extract the payload from anywhere in the chain.
if (auto* http = errors::As<HttpError>(err)) {
    std::cerr << "HTTP " << http->status_code
              << " from " << http->url << std::endl;
}
```

Payloads survive wrapping -- `As` walks the full chain:

```cpp
auto inner = errors::NewWithPayload("db timeout", RetryInfo{3, 5s});
auto outer = errors::Wrapf(inner, "loading user profile");

// Still reachable:
auto* retry = errors::As<RetryInfo>(outer);
```

You can also construct payloads in-place:

```cpp
auto err = errors::NewWithPayload(
    std::in_place_type<HttpError>, "request failed", 503, "/v1/users");
```

### Wrapping with payloads

Combine wrapping and payload attachment in one step:

```cpp
auto err = errors::WrapWithPayload(
    inner_err, "api gateway error", GatewayInfo{region, timestamp});
```

### Serializable payloads

When a payload is a protobuf message (or any type satisfying the
`WireSerializable` concept -- i.e., it has `SerializeAsString()` and
`GetTypeName()` methods), the error gains serialization capabilities:

```cpp
// Attach a protobuf message as a payload.
LoginRequest req;
req.set_user("alice");
req.set_ip_address("10.0.0.1");
auto err = errors::NewWithPayload("login failed", req);
```

**Check if an error can be serialized** (all layers are dynamic, all payloads
are wire-serializable):

```cpp
if (errors::IsSerializable(err)) {
    // Safe to serialize for logging, storage, or wire transport.
}
```

**Get a human-readable dump** including payload contents -- ideal for logging.
The caller does not need to know the payload type:

```cpp
// Generic error logging middleware:
void LogError(const errors::Error& err) {
    LOG(ERROR) << errors::DebugString(err);
    // => "login failed [test.LoginRequest: user: "alice" ip_address: "10.0.0.1"]"
}
```

This solves a common observability problem: structured payloads carry rich
context (entire request protos, trace metadata, etc.) but `message()` only
returns the string chain. `DebugString` makes the full structured context
visible at generic error boundaries -- middleware, logging interceptors, error
reporters -- without requiring `As<T>()` with a known type.

**Serialize for storage or wire transport:**

```cpp
std::string bytes = errors::Serialize(err);
// Store in a database, send over gRPC, write to a log file...

// On the receiving side:
errors::Error restored = errors::Deserialize(bytes);
restored.message();  // => "login failed"

// Payloads arrive as SerializedPayload:
auto* sp = errors::As<errors::SerializedPayload>(restored);
LoginRequest req;
req.ParseFromString(sp->data);  // reconstruct the original proto
```

Note: sentinel errors are not serializable (they represent process-local
identity). `IsSerializable` returns `false` if any layer in the chain is a
sentinel.

### Inspecting individual layers

`what()` returns the message for a single layer (without walking the chain),
and `Unwrap()` returns the next error in the chain:

```cpp
auto inner = errors::New("disk full");
auto outer = errors::Wrapf(inner, "saving config");

outer.what();       // => "saving config"  (just this layer)
outer.message();    // => "saving config: disk full"  (full chain)

auto* next = outer.Unwrap();   // => pointer to the inner error
next->what();                  // => "disk full"
next->Unwrap();                // => nullptr (end of chain)
```

`what()` and `Unwrap()` on a nil error return an empty `string_view` and
`nullptr` respectively.

### `std::format` support

`errors::Error` has a `std::formatter` specialization, so it can be used
directly with `std::format`, `std::print`, and any other formatting API:

```cpp
auto err = errors::Wrapf(kErrNotFound, "loading user {}", user_id);
std::println("request failed: {}", err);
// => "request failed: loading user 42: resource not found"
```

### `Result<T>`

`Result<T>` is a discriminated union that holds either a value of type `T`
(success) or an `Error` (failure). Include `errors/result.h` to use it:

```cpp
#include "errors/result.h"

errors::Result<int> ParsePort(std::string_view s) {
    int port = /* ... */;
    if (port <= 0 || port > 65535)
        return errors::New("invalid port");
    return port;
}

auto result = ParsePort("8080");
if (result) {
    std::cout << "port = " << *result << std::endl;
} else {
    std::cerr << result.error().message() << std::endl;
}
```

`Result<T>` is `[[nodiscard]]`, supports implicit construction from both `T`
and `Error`, and provides `ok()`, `value()`, `error()`, `operator*`, and
`operator->`. It is header-only and does not require linking.

### `Result<void>`

For functions that can fail but have no value to return, use `Result<void>`:

```cpp
errors::Result<void> SaveConfig(const Config& cfg) {
    auto err = WriteFile(cfg.path(), cfg.Serialize());
    if (err)
        return err;
    return {};  // success
}

auto result = SaveConfig(cfg);
if (!result) {
    std::cerr << result.error().message() << std::endl;
}
```

`Result<void>` is 8 bytes -- the same size as a bare `Error`. Default
construction is success (nil error); construction from an `Error` is failure.
There is no `value()` method.

### Error propagation macros

Three macros reduce boilerplate when propagating errors up the call stack.

#### `ERRORS_RETURN_IF_ERROR(expr)`

Evaluates `expr` (which must return an `Error`). If the error is non-nil,
returns it from the enclosing function. Works in functions returning `Error`
or `Result<void>`:

```cpp
errors::Error DoMultipleSteps() {
    ERRORS_RETURN_IF_ERROR(Step1());
    ERRORS_RETURN_IF_ERROR(Step2());
    ERRORS_RETURN_IF_ERROR(Step3());
    return {};
}
```

Defined in `errors/error.h`.

#### `ERRORS_ASSIGN_OR_RETURN(lhs, expr)`

Evaluates `expr` (which must return a `Result<T>`). On success, assigns the
value to `lhs`; on failure, returns the error from the enclosing function:

```cpp
errors::Result<Config> LoadAndValidate() {
    ERRORS_ASSIGN_OR_RETURN(auto data, ReadFile("config.yaml"));
    ERRORS_ASSIGN_OR_RETURN(auto cfg, Parse(data));
    return cfg;
}
```

`lhs` can be `auto val`, `auto& val`, or an existing variable name.
Defined in `errors/result.h`.

#### `ERRORS_TRY(expr)` (GCC/Clang only)

A statement-expression macro that evaluates to the unwrapped value on success,
or returns the error on failure. Can be used inline in expressions:

```cpp
errors::Result<std::string> BuildGreeting() {
    auto name = ERRORS_TRY(LookupName(user_id));
    return "Hello, " + name;
}
```

This is a GCC extension (supported by Clang) and is only available when
`__GNUC__` is defined. Defined in `errors/result.h`.

## API reference

| Function | Description |
|---|---|
| `errors::New(msg)` | Create a new error with a message. |
| `errors::Errorf(fmt, args...)` | Create a new error with `std::format`. |
| `errors::Wrap(err, msg)` | Wrap an existing error with a plain message (no formatting). |
| `errors::Wrapf(err, fmt, args...)` | Wrap an existing error with formatted context. |
| `errors::Is(err, target)` | Check if `target` appears anywhere in `err`'s chain. |
| `errors::As<T>(err)` | Extract a `T*` payload from `err`'s chain, or `nullptr`. |
| `errors::NewWithPayload(msg, payload)` | Create an error with an attached typed payload. |
| `errors::WrapWithPayload(err, msg, payload)` | Wrap an error and attach a payload. |
| `errors::IsSerializable(err)` | Check if all layers in `err`'s chain are serializable. |
| `errors::DebugString(err)` | Full chain message with inline payload debug info. |
| `errors::Serialize(err)` | Serialize the error chain to binary format. |
| `errors::Deserialize(data)` | Reconstruct an error chain from serialized bytes. |
| `Error::Nil()` | Returns a reference to the global nil error. |
| `Error::message()` | Returns the full colon-separated message chain. |
| `Error::what()` | Returns the message for this single layer only. |
| `Error::Unwrap()` | Returns a pointer to the next error in the chain, or `nullptr`. |
| `Result<T>` | Discriminated union of `T` (success) or `Error` (failure). |
| `Result<void>` | Success/failure without a value (8 bytes, nil Error = success). |
| `SerializedPayload` | Container for deserialized payload data (type URL + bytes). |
| `std::formatter<errors::Error>` | `std::format` support -- formats as `err.message()`. |
| `ERRORS_DEFINE_SENTINEL(name, msg)` | Define a named sentinel error constant. |
| `ERRORS_RETURN_IF_ERROR(expr)` | Return early if `expr` yields a non-nil `Error`. |
| `ERRORS_ASSIGN_OR_RETURN(lhs, expr)` | Unwrap a `Result<T>` or return the error. |
| `ERRORS_TRY(expr)` | (GCC/Clang) Unwrap a `Result<T>` inline or return the error. |

## Performance

### Performance characteristics

- `sizeof(Error)` is **8 bytes** (a single tagged pointer).
- Sentinel errors involve **zero heap allocation** -- they are `constinit`
  globals.
- Short error messages (up to 23 bytes) use **small-string optimization** and
  avoid heap allocation entirely.
- **Copies are O(1)** via atomic reference counting. Only mutable access
  (non-const `As<T>`) triggers a copy-on-write clone.
- Error comparison (`==`, and `Is` per chain link) is a **single integer
  comparison**.
- Move operations are a pointer copy + zero. No allocation, no branching
  beyond a null check.
- **No RTTI required.** Type matching uses static-address type IDs, so the
  library works with `-fno-rtti`.
- No exceptions are thrown. Error propagation is a normal return.

### Benchmarks

All numbers below are **median CPU time** from Release builds (clang++-20,
`-O2`, Google Benchmark v1.9.1, 3 repetitions). Run on a Linux x86-64 machine.
Your results will vary with hardware, but the relative ratios are the point.

#### Core operations

| Operation | CPU (ns) | Notes |
|---|--:|---|
| Nil creation | 3.2 | Zero-init 8 bytes |
| Sentinel copy | 3.4 | `uintptr_t` copy, zero alloc |
| `New` (SSO, 9 bytes) | 33 | Single heap alloc for impl, SSO string |
| `New` (heap, 50 bytes) | 58 | Heap alloc for impl + string buffer |
| `Errorf` (SSO) | 182 | `std::format` + SSO path |
| `Errorf` (heap) | 186 | `std::format` + heap path |
| `NewWithPayload` | 61 | Typed payload + SSO |
| `Wrapf` (SSO) | 100 | Wrap sentinel with short context |
| `Wrapf` (heap) | 140 | Wrap sentinel with 50-char context |

#### Inspection and traversal

| Operation | CPU (ns) | Notes |
|---|--:|---|
| `operator bool` | 1.2 | Single integer compare to zero |
| `operator==` | 1.9 | Single integer compare |
| `what()` (dynamic) | 2.7 | Single-layer message, virtual dispatch |
| `what()` (sentinel) | 7.5 | Single-layer message, tag clearing |
| `Is` (depth 1) | 6 | Walk 1 link, match found |
| `Is` (depth 10) | 44 | Walk 10 links, match at root |
| `As<T>` (depth 1) | 13 | Type-ID check at each link |
| `As<T>` (depth 10) | 97 | 10 links, payload at root |
| `Unwrap()` walk (depth 10) | 31 | Manual chain traversal, ~3 ns/hop |
| `message()` (single) | 12 | Build string from one layer |
| `message()` (depth 10) | 267 | Concatenate 11 layers |

#### Copy, move, and copy-on-write

| Operation | CPU (ns) | Notes |
|---|--:|---|
| Copy sentinel | 3.4 | Copies tagged pointer (no alloc) |
| Copy dynamic (SSO) | 14 | Atomic refcount increment (O(1), no clone) |
| Copy dynamic (heap) | 14 | Atomic refcount increment (O(1), no clone) |
| Copy 50-deep chain | 14 | O(1) regardless of chain depth |
| COW read (copy + const `As`) | 15 | Shared read, no clone triggered |
| COW mutate (copy + non-const `As`) | 56 | Clone on first mutable access |

Copies share state via atomic reference counting. Mutation through non-const
`As<T>()` triggers copy-on-write, cloning only the outermost layer on demand.
The copy cost is constant regardless of chain depth.

A/B comparison on the same machine (using `noinline` wrappers to prevent
compiler allocation elision):

| Chain depth | Old (deep clone) | New (refcount) | Speedup |
|--:|--:|--:|---|
| 1 | 33 ns | 14 ns | 2.4x |
| 5 | 150 ns | 14 ns | 11x |
| 10 | 340 ns | 14 ns | 24x |
| 50 | 1,898 ns | 14 ns | 136x |

#### `Result<T>` vs `std::expected`

| Operation | `Result<T>` (ns) | `expected<T,string>` (ns) |
|---|--:|--:|
| Success construction | 0.75 | 0.99 |
| Failure construction | 38 | 2.3 |
| Failure (sentinel) | 8.2 | -- |
| `ok()` / `has_value()` | 0.74 | 0.37 |
| Value access (`*r`) | 0.37 | 0.37 |

`Result<T>` failure is more expensive because it heap-allocates an `Error`
(which carries chaining, payloads, and identity). Using a sentinel error
instead of `New()` brings the failure cost to ~8 ns. Success paths are
equivalent.

#### Compared to standard C++ alternatives

**Error creation and return** (through a `noinline` function):

| Mechanism | CPU (ns) | vs `errors::New` |
|---|--:|---|
| Raw integer return | 0.4 | -- |
| `std::error_code` | 1.1 | -- |
| **Sentinel return** | **4.1** | **0.1x** |
| `std::expected<int, string>` | 10 | 0.3x |
| **`errors::New`** | **35** | **1x** |
| `throw` + `catch` | 2,991 | 85x slower |

**Error identity check:**

| Mechanism | CPU (ns) | vs `errors::Is` |
|---|--:|---|
| Raw int `==` | 0.4 | -- |
| `error_code ==` | 0.4 | -- |
| `expected::has_value()` | 0.7 | -- |
| `expected` string compare | 1.5 | -- |
| **`errors::Is` (sentinel)** | **2.6** | **1x** |
| `throw` + `catch` (as "type check") | 2,101 | 808x slower |

**Success path (no error):**

| Mechanism | CPU (ns) |
|---|--:|
| Raw int `== 0` | 0.4 |
| `error_code` bool | 0.4 |
| `expected::has_value()` | 0.7 |
| **`errors::Error` bool** | **1.5** |

**Error propagation (wrapping/re-throwing), depth 10:**

| Mechanism | CPU (ns) | vs `Wrapf` |
|---|--:|---|
| `expected` string concat | 534 | 0.5x |
| **`errors::Wrapf` chain** | **997** | **1x** |
| `throw` + re-`throw` | 9,658 | 9.7x slower |

`errors::Error` sits in the same performance tier as `std::expected` for
error creation and propagation, while providing chaining, identity
preservation, and structured payloads that `expected` does not support. Both
are orders of magnitude faster than exceptions on the error path.

#### Running benchmarks yourself

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-20 errors
cmake --build build --target error_benchmark
./build/error_benchmark
```

## License

MIT License. Copyright (c) 2026 Amaltas Bohra. See [LICENSE](LICENSE) for details.
