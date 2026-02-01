# Error Chaining Was the Missing Piece in C++ Error Handling.

Every C++ codebase we worked on has its own answer to "how do we handle errors?" Exceptions in one module, `std::error_code` in another, `std::expected` in the newer code, raw integers in the legacy paths.

But the real problem isn't the variety. It's that **every one of these mechanisms loses context as the error moves up the call stack.**

A database driver returns `"connection refused"`. Three layers up, an HTTP handler catches... `"connection refused"`. Which database? Which query? Which user request triggered it? The information existed at each layer. It just had no place to go.

The usual workaround is string concatenation -- manually prepending context at each layer. It works, but it destroys structure. You can't programmatically ask "was the root cause a connection refusal?" once everything is mashed into a single string. You've traded identity for context.

## The design in 30 seconds

```cpp
ERRORS_DEFINE_SENTINEL(kErrConnRefused, "connection refused");

errors::Error Connect(std::string_view host) {
    if (!TcpConnect(host))
        return kErrConnRefused;             // sentinel -- zero heap alloc
    return {};                              // nil -- success
}

errors::Error UserRepo::FindById(int64_t id) {
    auto err = db_.Connect("db-primary:5432");
    if (err)
        return errors::Wrapf(err, "user repo: query user {}", id);
    return {};
}

errors::Error ProfileService::Load(int64_t user_id) {
    auto err = repo_.FindById(user_id);
    if (err)
        return errors::Wrapf(err, "loading profile");
    return {};
}
```

The final error message reads:

```
loading profile: user repo: query user 42: connection refused
```

Every layer's context. One line of wrapping per call site. And the original sentinel (`kErrConnRefused`) is still reachable through any depth of wrapping:

```cpp
if (errors::Is(err, kErrConnRefused)) {
    // still matches, no matter how many layers of Wrapf
}
```

## The gap this fills

C++ has good building blocks for error handling. `std::expected` gives you a clean success-or-failure return. `std::error_code` gives you efficient, non-throwing error codes. Status-style libraries give you a uniform error type with canonical categories. Each solves real problems.

What none of them provide is **composable error context** -- the ability to wrap an error with additional information at each layer while preserving the original cause's identity and any structured data it carries.

That's the specific capability this library adds:

- **Error chaining.** Wrap with `Wrapf` at each call site. The full context trail is preserved as a linked chain, not a flattened string.
- **Identity through wrapping.** Define sentinel errors for well-known conditions. `Is()` walks the chain to find them, no matter how many layers deep.
- **Typed payloads through wrapping.** Attach a struct (HTTP status, trace ID, retry metadata) at any layer. `As<T>()` walks the chain to find it.

These three features work together. An error can carry a sentinel identity *and* a typed payload *and* multiple layers of human-readable context, all at the same time. Any layer in the stack can inspect any of these without knowing what the other layers added.

## Cutting the boilerplate

The most common complaint about value-based error handling in C++ is the ceremony. Every call site needs a temporary, an `if` check, and a return. Three lines, repeated everywhere. Multiply by a hundred call sites and it dominates the code.

The library ships five macros that collapse the pattern to a single line.

**Propagate an error without a value:**

```cpp
errors::Error Initialize() {
    ERRORS_RETURN_IF_ERROR(LoadConfig());
    ERRORS_RETURN_IF_ERROR(ConnectDatabase());
    ERRORS_RETURN_IF_ERROR(StartServer());
    return {};
}
```

**Unwrap a `Result<T>` or return the error:**

```cpp
errors::Result<Config> LoadAndValidate() {
    ERRORS_ASSIGN_OR_RETURN(auto data, ReadFile("config.yaml"));
    ERRORS_ASSIGN_OR_RETURN(auto cfg, Parse(data));
    return cfg;
}
```

**Unwrap inline in an expression (GCC/Clang):**

```cpp
errors::Result<std::string> BuildGreeting() {
    auto name = ERRORS_TRY(LookupName(user_id));
    return "Hello, " + name;
}
```

**Propagate with wrapping context (the safe way):**

```cpp
errors::Error LoadProfile(int64_t user_id) {
    ERRORS_RETURN_IF_ERROR_WRAPF(repo_.FindById(user_id),
                                 "loading profile for user {}", user_id);
    return {};
}

errors::Result<Config> LoadAndValidate(std::string_view path) {
    ERRORS_ASSIGN_OR_RETURN_WRAPF(auto data, ReadFile(path),
                                  "reading config from {}", path);
    ERRORS_ASSIGN_OR_RETURN_WRAPF(auto cfg, Parse(data), "parsing config");
    return cfg;
}
```

The `_WRAPF` variants wrap the error with `Wrapf` before returning. This avoids a subtle bug with the naive composition `ERRORS_RETURN_IF_ERROR(errors::Wrap(expr, "msg"))` -- which calls `Wrap` unconditionally, turning a nil (success) error into a non-nil one.

`ERRORS_TRY` uses a GCC statement expression -- non-standard, but supported by both GCC and Clang. `ERRORS_ASSIGN_OR_RETURN` works everywhere.

For functions that can fail but have no value to return, there's `Result<void>` -- 8 bytes, same size as a bare `Error`. Default construction is success; construction from `Error` is failure. No `bool` flag needed -- the nil state of the Error *is* the success state.

## What's under the hood

The `Error` type is **8 bytes** -- a tagged `uintptr_t`:

- **`0`** means nil (no error). Checking for success is a compare-to-zero.
- **Odd bit set** means sentinel -- a non-owning pointer to a `constinit` global. Returning a sentinel is copying an integer. Zero allocation.
- **Even, non-zero** means a heap-allocated dynamic error with an ownership chain.

Dynamic errors use a manual SSO (Small String Optimization) with a 23-byte inline buffer. Most error messages ("connection refused", "permission denied", "not found") fit without a separate allocation. For longer messages, it falls back to a heap buffer.

Copies are O(1) via intrusive atomic reference counting. Mutation through non-const `As<T>()` triggers copy-on-write, cloning only the outermost layer on demand. No `shared_ptr`, no control block indirection.

Payloads use a `DetailedError<T>` template that stores the typed payload inline. `As<T>` walks the chain using static-address type IDs -- no RTTI, no `dynamic_cast`, no string keys.

## The benchmarks

Also includes a comprehensive benchmark suite (69 measurements) comparing against `std::expected`, `std::error_code`, raw integers, and `throw`/`catch` with `std::runtime_error`.

All numbers are median CPU nanoseconds, Release build, Clang 20, `-O2`.

**Creating and returning an error (through a `noinline` function):**

| Mechanism | CPU (ns) |
|---|--:|
| Raw integer return | 0.4 |
| `std::error_code` | 1.1 |
| Sentinel return | 4 |
| `std::expected<int, string>` | 11 |
| `errors::New("msg")` | 31 |
| `throw` + `catch` | 3,277 |

Sentinel errors -- the ones you'd use for well-known conditions -- cost **4 ns**. That's in `error_code` territory. Dynamic errors with `New()` cost 31 ns, which includes a heap allocation. Exceptions are two orders of magnitude slower.

**Checking error identity:**

| Mechanism | CPU (ns) |
|---|--:|
| Raw int `==` | 0.4 |
| `expected::has_value()` | 0.4 |
| `error_code` compare | 0.8 |
| `errors::Is` (sentinel) | 2.8 |
| `throw` + `catch` as type check | 2,131 |

`Is()` on a sentinel is a pointer comparison per chain link. At depth 1 it's 2.8 ns. At depth 10 it's 42 ns -- still sub-microsecond. Exception-based "type checking" via catch clauses is three orders of magnitude slower.

**Error propagation -- wrapping 10 layers deep:**

| Mechanism | CPU (ns) |
|---|--:|
| `expected` string concat | 599 |
| `errors::Wrapf` chain | 931 |
| `throw` + re-`throw` | 10,061 |

`expected` is faster here because it's doing simple string concatenation -- but concatenation destroys structure. You can't walk back through a concatenated string to find the root cause or check its identity. With `Wrapf`, each layer is a distinct node in the chain. You pay ~90 ns per layer, but you preserve the full structure for programmatic inspection. Exceptions cost **10.8x** more and still don't give you structured traversal.

**The success path (no error at all):**

| Mechanism | CPU (ns) |
|---|--:|
| Raw int `== 0` | 0.4 |
| `expected::has_value()` | 0.4 |
| `error_code` bool | 0.8 |
| `errors::Error` bool | 1.5 |

Checking for success is a single integer compare-to-zero. The 1.5 ns includes non-inlined function call overhead; with LTO this would be indistinguishable from a raw comparison.

**Macro overhead (through `noinline` functions):**

| Macro | Success (ns) | Failure (ns) |
|---|--:|--:|
| `ERRORS_RETURN_IF_ERROR` | 6 | 36 |
| `ERRORS_ASSIGN_OR_RETURN` | 3 | 47 |
| `ERRORS_TRY` | 4 | 47 |

On the success path, the macros add single-digit nanoseconds of overhead. The failure path cost is dominated by `errors::New()` (heap allocation), not the macro itself. The macros are syntactic sugar -- they generate the same code you'd write by hand.

## What this adds up to

Here's where the different C++ error mechanisms stand on the capabilities that matter for production debugging:

| Feature | `error_code` | `expected` | exceptions | `errors::Error` |
|---|---|---|---|---|
| Error chaining | No | No | No | **Yes** |
| Identity through wrapping | N/A | N/A | Lost on re-throw | **Preserved** |
| Typed payloads | No | Only what `E` holds | Via subclassing | **Native C++ types** |
| Uniform type | Yes | No (`E` varies) | No | **Yes** |
| Zero-cost sentinels | Predefined codes | N/A | No | **Yes (4 ns)** |
| User-defined error kinds | Via categories | N/A | Subclassing | **Unlimited sentinels** |
| Propagation macros | No | No | N/A (implicit) | **5 macros** |

The cost is `expected`-tier. The capability set is new to C++.

## The API

The entire public API fits on an index card:

```cpp
errors::New("msg")                              // create
errors::Errorf("port {} in use", 8080)          // create with std::format
errors::Wrap(err, "context")                    // wrap with plain message
errors::Wrapf(err, "context: {}", detail)       // wrap with std::format
errors::Is(err, kSentinel)                      // identity check through chain
errors::As<T>(err)                              // payload extraction through chain
errors::NewWithPayload("msg", MyStruct{...})    // create with typed payload
errors::WrapWithPayload(err, "msg", payload)    // wrap + attach payload
ERRORS_DEFINE_SENTINEL(kName, "message")        // define sentinel constant
err.message()                                   // full chain as string
if (err) { ... }                                // nil check

// Result<T> and Result<void>
errors::Result<int> r = 42;                     // success
errors::Result<int> r = errors::New("fail");    // failure
errors::Result<void> r;                         // void success (8 bytes)

// Propagation macros
ERRORS_RETURN_IF_ERROR(expr)                    // return Error if non-nil
ERRORS_RETURN_IF_ERROR_WRAPF(expr, fmt, ...)    // wrap + return if non-nil
ERRORS_ASSIGN_OR_RETURN(auto val, expr)         // unwrap Result<T> or return
ERRORS_ASSIGN_OR_RETURN_WRAPF(val, expr, ...)   // unwrap or wrap + return
ERRORS_TRY(expr)                                // unwrap inline (GCC/Clang)
```

If you've used Go's error handling, you already know this API. If you haven't, it takes about five minutes to learn.

## Payloads in practice

Structured payloads go beyond what string messages can carry. Attach domain-specific data at the point of failure and extract it at any handling boundary:

```cpp
struct HttpError {
    int status_code;
    std::string url;
};

// At the failure site:
auto err = errors::NewWithPayload(
    "request failed", HttpError{503, "/v1/users"});

// At any handler, through any depth of wrapping:
if (auto* http = errors::As<HttpError>(err)) {
    std::cerr << "HTTP " << http->status_code
              << " from " << http->url << std::endl;
}
```

Payloads are native C++ types. No serialization to attach them, no deserialization to read them, no string keys. When the payload satisfies a `WireSerializable` concept (e.g., a protobuf message), the library can also serialize the entire error chain -- message, payloads, and all -- for logging or wire transport. But that's opt-in, not required.

## Try it

The library is C++23, tested with Clang 20 and GCC 14. It's a single header + source file, integrated via CMake `add_subdirectory`. No dependencies beyond the standard library.

The code, benchmarks, and full documentation are on GitHub: [link]

If you've ever been frustrated by errors that lose their story the moment they leave the function that created them, give this a look. I'd love to hear what you think.
