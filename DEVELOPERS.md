# Developer & Maintainer Guide

Internal reference for anyone modifying or extending the `errors` library.

## Design philosophy

This library exists because C++ lacks a standard, ergonomic, uniform error
type. The design is lifted directly from Go's `error` interface and its
standard library helpers (`errors.New`, `fmt.Errorf`, `errors.Is`,
`errors.As`). The goals, in priority order:

1. **One return type.** Every fallible function returns `errors::Error`. Callers
   never see templates, variants, or error-code enums in function signatures.
   This eliminates the combinatorial explosion that `std::expected<T,E>` causes
   when different subsystems choose different `E` types.

2. **Value semantics, minimal size.** `Error` is 8 bytes -- the same size as a
   raw pointer. It is cheap to return, copy, and move. Copies share state via
   intrusive atomic reference counting (no `std::shared_ptr`, no control block).
   Mutable access triggers copy-on-write.

3. **Zero-cost sentinels.** Named error constants like `kErrNotFound` are
   `constinit` globals. Creating or comparing them involves no allocation and
   no branching beyond a single integer comparison.

4. **Composable context.** Wrapping adds a human-readable layer without
   destroying the original cause. `Is` and `As` walk the full chain.

5. **Structured payloads.** Arbitrary typed data can be attached to any error
   and extracted later, without downcasting or `dynamic_cast`.

6. **Serializable payloads.** Payloads satisfying the `WireSerializable`
   concept (e.g., protobuf messages) can be serialized along with the full
   error chain for logging, storage, or wire transport -- without introducing
   a protobuf dependency into the core library.

## Architecture

```
errors/
  error.h              Public header (the primary file users include)
  error.cc             Non-template method definitions, factory functions
  error_internal.h     Private header: ErrorImpl, DynamicError, StaticSentinelError, DetailedError<T>
  result.h             Header-only Result<T> discriminated union (optional include)
  error_test.cc        Unit tests (Google Test)
  error_benchmark.cc   Micro-benchmarks (Google Benchmark)
  CMakeLists.txt       Build configuration (C++23, fetches Google Test + Benchmark)
```

### Header structure: the lock-and-key pattern

`error_internal.h` contains implementation details that must not be included
directly by users. It is guarded by a preprocessor check:

```cpp
#ifndef ERRORS_PUBLIC_HEADER_INCLUDED_
#error "Private header. Please include 'errors/error.h' instead."
#endif
```

`error.h` defines the macro before including the internal header and undefines
it immediately after. This ensures the internal header can only be reached
through the public header, while still allowing template code in `error.h` to
reference internal types.

### Class hierarchy

```
ErrorImpl (abstract base)
  |
  +-- StaticSentinelError      consteval, stores const char*, no allocation
  |
  +-- DynamicError             runtime, SSO string, owns inner Error chain
        |
        +-- DetailedError<T>   extends DynamicError with typed payload
```

`Error` is a handle (8-byte tagged pointer) that owns or borrows one of these.

## Tagged pointer encoding

`Error` stores a single `uintptr_t bits_`:

| `bits_` value       | Kind     | Pointer recovery                                 | Ownership |
|----------------------|----------|--------------------------------------------------|-----------|
| `0`                  | Nil      | `nullptr`                                        | N/A       |
| Odd (`bits_ & 1`)   | Sentinel | `std::bit_cast<const ErrorImpl*>(bits_ & ~1)` | Borrowed  |
| Even, non-zero       | Dynamic  | `std::bit_cast<ErrorImpl*>(bits_)`            | Owned     |

**Why this works:** `ErrorImpl` has a vtable, so `alignof(ErrorImpl) >= 8` on
all 64-bit platforms (verified by a `static_assert` in `error.cc`). Every valid
`ErrorImpl*` has its low 3 bits zero. Setting bit 0 for sentinels creates a
value that can never collide with a dynamic pointer (even) or nil (zero).

**Consequences:**

- `operator==` is a single integer comparison. No tag-checking branches.
- `get_impl()` masks off bit 0 with `bits_ & ~uintptr_t{1}` and casts. This
  returns the correct pointer for all three kinds (nil yields `nullptr`, sentinel
  clears the tag bit, dynamic is already clean).
- Destruction checks `is_dynamic()` (two conditions: non-zero and even) before
  calling `delete`.

### Trade-off: sentinel constructor is not constexpr

The sentinel constructor uses `std::bit_cast`, which cannot appear in a
constant expression. The previous 16-byte layout had a `constexpr` sentinel
constructor. The macro `ERRORS_DEFINE_SENTINEL` declares the `Error` as
`inline const` (not `constinit`), so the sentinel `Error` is dynamically
initialized at startup (a single OR instruction). The `StaticSentinelError`
impl itself remains `constinit consteval` and is placed in read-only data.

## Small-string optimization (SSO)

`DynamicError` stores messages inline for strings up to 23 bytes:

```cpp
static constexpr size_t SSO_CAPACITY = 23;
union {
    char sso_buffer_[SSO_CAPACITY + 1];  // +1 for null terminator
    char* heap_buffer_;
} data_;
size_t size_;
```

The 23-byte threshold was chosen to keep the union the same size as a pointer
plus padding on 64-bit platforms, while covering the majority of error messages
(most are short English strings like "permission denied" or "not found").

Messages longer than 23 bytes are heap-allocated. The `set_string` method
handles both paths. Rule-of-5 is manually implemented because the union cannot
have a non-trivial destructor.

## Error chain

Errors form a singly-linked chain through the `inner_` member of
`DynamicError`. Each node stores its own message layer. The public
`Error::message()` method walks the chain and concatenates all layers with
`": "` separators, producing output like:

```
api gateway error: service layer failure: permission denied
```

`Is` walks the chain comparing each node against a target (single integer
comparison per node, plus a virtual `matches()` hook for extensibility).
`As<T>` walks the chain calling `get_payload(type_id<T>())` on each node.

`Wrap(err, msg)` is a non-template alternative to `Wrapf` for cases where no
`std::format` formatting is needed. It avoids the ~70 ns overhead of
`std::format` by constructing a `DynamicError` directly from a
`std::string_view`.

The public `what()` method returns the message for a single layer (delegates
to `ErrorImpl::message_view()`). `Unwrap()` returns a pointer to the next
error in the chain (delegates to the private `unwrap()`).

## Payload system

`DetailedError<T>` extends `DynamicError` with a `T details_` member. Payload
extraction uses a static-address type ID (no RTTI required):

```cpp
// In errors::internal:
using TypeId = const void*;
template <typename T>
TypeId type_id() noexcept {
    static constexpr char id = '\0';
    return &id;
}

// In DetailedError<T>:
void* get_payload(TypeId id) noexcept override {
    if (id == type_id<T>()) return &details_;
    return nullptr;
}
```

Each `type_id<T>()` instantiation produces a unique address, giving the same
effect as `typeid(T)` but without requiring RTTI. This allows the library to
be compiled with `-fno-rtti`. The caller asks for `T` by type, and `As<T>`
walks the chain trying each node.

Payloads are accessible through any depth of wrapping because `As` iterates
the full chain.

### Serialization hooks

`ErrorImpl` declares four virtual methods for serialization support:

```cpp
virtual bool is_serializable() const noexcept;   // can this layer be serialized?
virtual std::string serialize_payload() const;    // binary payload bytes
virtual std::string payload_type_url() const;     // type identifier string
virtual std::string payload_debug_string() const; // human-readable payload
```

Default implementations return `false` / empty. Overrides:

| Subclass | `is_serializable()` | Payload methods |
|---|---|---|
| `StaticSentinelError` | `false` (inherits default) | N/A |
| `DynamicError` | `true` | N/A (no payload, defaults are fine) |
| `DetailedError<T>` | `WireSerializable<T>` | Delegates to `T` via `if constexpr` |

Two concepts in `error_internal.h` enable compile-time dispatch without
requiring a protobuf header dependency:

```cpp
template <typename T>
concept WireSerializable = requires(const T& t) {
  { t.SerializeAsString() } -> std::convertible_to<std::string>;
  { t.GetTypeName() } -> std::convertible_to<std::string>;
};

template <typename T>
concept HasDebugString = requires(const T& t) {
  { t.ShortDebugString() } -> std::convertible_to<std::string>;
};
```

`DetailedError<T>` uses `if constexpr (WireSerializable<T>)` to implement the
virtual methods. This means the serialization code for `T` is only compiled
when `T` actually satisfies the concept. No protobuf headers are needed in
the core library; any type that duck-types the interface works.

**`DebugString(err)`** walks the chain like `message()` but appends payload
debug info in brackets: `"msg [type: debug_info]: inner_msg"`. For layers
without a serializable payload, the output is identical to `message()`.

**`Serialize(err)` / `Deserialize(data)`** use a simple length-prefixed binary
format. Each layer is encoded as three length-prefixed strings (message, type
URL, payload bytes). Deserialized payloads arrive as `SerializedPayload`
(a struct storing the type URL and raw bytes) rather than the original C++
type. The caller uses `As<SerializedPayload>(err)` and parses manually.

**Why sentinels are not serializable:** Sentinel identity is a process-local
pointer comparison. There is no mechanism to reconstruct that identity in
another process. `IsSerializable()` returns `false` if any layer in the chain
is a sentinel.

**Why not a combined serialization virtual?** Combining `serialize_payload()`
and `payload_type_url()` into a single virtual call (returning a struct with
both values) was considered and rejected. The gain is ~2-5 ns per layer from
one fewer virtual dispatch, but the individual methods are also needed by
`DebugString()` (which calls `payload_type_url()` and
`payload_debug_string()` separately). A combined method would not replace
the existing virtuals, only add a third path, increasing API complexity for
negligible performance benefit.

## Reference counting and copy-on-write

`ErrorImpl` carries an intrusive `mutable std::atomic<uint32_t> ref_count_`
(initialized to 1). This makes `Error` copies O(1):

| Operation | Before (v1) | After (v2) |
|---|---|---|
| Copy ctor | Deep clone via `clone()` | `add_ref()` (atomic increment) |
| Destructor | `delete impl` | Decrement; delete only when count reaches 0 |
| `get_mutable_impl()` | Return raw pointer | If `use_count() > 1`: COW clone, release old, update `bits_` |

**COW semantics:** Non-const `As<T>()` calls `get_mutable_impl()`, which
checks the reference count. If shared (`use_count() > 1`), it clones the
outermost layer, releases the old reference, and updates the handle. The
clone's inner chain is still shared (the copy ctor of `DynamicError` copies
`inner_` via `Error(const Error&)`, which does `add_ref()`). Further mutable
access into inner layers triggers COW lazily at each level.

**Sentinel safety:** Sentinels are identified by the odd-tagged pointer and
never reach refcount code paths (`is_dynamic()` returns false). The
`std::atomic<uint32_t>{1}` member is constexpr in C++20+, so
`StaticSentinelError`'s `consteval` constructor still compiles.

**Thread safety:** Copies are safe from multiple threads (atomic refcount).
Concurrent read+write to the *same* `Error` object is not safe, matching the
thread safety model of `std::string` and `absl::Status`. This is documented
in a comment block above the `Error` class in `error.h`.

## Is() extensibility

`ErrorImpl` provides a virtual `matches(const Error& target)` hook that
defaults to returning `false`. After the pointer-equality check in `Is()`
fails at each chain node, `matches()` is called on the node's impl. This
allows future custom error types to define domain-specific matching logic
without modifying the core `Is()` implementation.

## Result\<T\>

`errors/result.h` provides a header-only `Result<T>` discriminated union:

- `union { T value; Error error; }` + `bool has_value_`
- `[[nodiscard]]` on the class to encourage inspection
- Implicit construction from `T` (success) and `Error` (failure)
- `static_assert` prevents `Result<Error>` and `Result<T&>`
- Asserts that the `Error` is non-nil on failure construction
- Full Rule of 5 using `std::construct_at` / `std::destroy_at`

No changes to `CMakeLists.txt` are needed since `result.h` is header-only.

### Result\<void\> specialization

`Result<void>` is a partial specialization that holds no value -- just
success or failure. It stores a single `Error` member (no `bool has_value_`
needed). Nil `Error` *is* the success state. Total size: **8 bytes**, same as
a bare `Error`.

The constructor from `Error` is implicit (not explicit) so that functions can
`return err;` naturally. An `assert` checks that the `Error` is non-nil on
the failure path.

There is no `value()` method. The `error()` accessors assert on success (nil
error), matching the behavior of `Result<T>::error()` asserting on the success
state.

## Error propagation macros

Five macros in `error.h` and `result.h` reduce error-propagation boilerplate.

### `ERRORS_RETURN_IF_ERROR(expr)` (in `error.h`)

```cpp
#define ERRORS_RETURN_IF_ERROR(expr) \
  do {                               \
    if (auto _err_ = (expr); _err_) {\
      return _err_;                  \
    }                                \
  } while (false)
```

Design notes:

- `expr` must evaluate to an `Error`. The macro checks `operator bool` (true =
  non-nil = error) and returns it.
- The `do { ... } while (false)` idiom scopes the variable and ensures the
  macro behaves as a single statement (safe after `if` without braces).
- The enclosing function can return `Error` or `Result<void>` (since
  `Result<void>` is implicitly constructible from `Error`).
- **Do not pass `Result<void>` expressions** to this macro. `Result<void>`
  has the opposite bool semantics (`true` = success), so the early-return
  condition would be inverted.

### `ERRORS_RETURN_IF_ERROR_WRAPF(expr, ...)` (in `error.h`)

```cpp
#define ERRORS_RETURN_IF_ERROR_WRAPF(expr, ...)            \
  do {                                                      \
    if (auto _err_ = (expr); _err_) {                      \
      return errors::Wrapf(std::move(_err_), __VA_ARGS__); \
    }                                                       \
  } while (false)
```

Design notes:

- Same structure as `ERRORS_RETURN_IF_ERROR`, but wraps the error with
  `errors::Wrapf` before returning. The variadic arguments are forwarded
  directly to `Wrapf` (format string + args).
- This exists because the naive composition
  `ERRORS_RETURN_IF_ERROR(errors::Wrap(expr, "msg"))` is a bug: `Wrap` is
  called unconditionally, so a nil (success) error gets wrapped into a
  non-nil error. The `_WRAPF` variant only wraps when the error is non-nil.
- Uses `std::move` on the error to avoid a refcount bump before wrapping.

### `ERRORS_ASSIGN_OR_RETURN(lhs, expr)` (in `result.h`)

```cpp
#define ERRORS_ASSIGN_OR_RETURN(lhs, expr)                                    \
  auto ERRORS_CONCAT_INNER_(errors_result_, __LINE__) = (expr);               \
  if (!ERRORS_CONCAT_INNER_(errors_result_, __LINE__).ok())                   \
    return std::move(ERRORS_CONCAT_INNER_(errors_result_, __LINE__)).error();  \
  lhs = std::move(ERRORS_CONCAT_INNER_(errors_result_, __LINE__)).value()
```

Design notes:

- The temporary variable uses `__LINE__` to generate a unique name, preventing
  collisions when the macro is used multiple times in the same scope.
- The temporary **leaks into the enclosing scope** (it is not wrapped in a
  `do/while` because `lhs` may be a declaration like `auto val`). This is the
  same trade-off made by absl's `ASSIGN_OR_RETURN`.
- `lhs` can be `auto val`, `auto& val`, or an existing variable.
- Single evaluation of `expr`.
- **Cannot be used twice on the same line.** The `__LINE__`-based name would
  collide. In practice this is rarely an issue.

### `ERRORS_ASSIGN_OR_RETURN_WRAPF(lhs, expr, ...)` (in `result.h`)

```cpp
#define ERRORS_ASSIGN_OR_RETURN_WRAPF(lhs, expr, ...)                         \
  auto ERRORS_CONCAT_INNER_(errors_result_, __LINE__) = (expr);               \
  if (!ERRORS_CONCAT_INNER_(errors_result_, __LINE__).ok())                   \
    return errors::Wrapf(                                                     \
        std::move(ERRORS_CONCAT_INNER_(errors_result_, __LINE__)).error(),    \
        __VA_ARGS__);                                                         \
  lhs = std::move(ERRORS_CONCAT_INNER_(errors_result_, __LINE__)).value()
```

Design notes:

- Same structure as `ERRORS_ASSIGN_OR_RETURN`, but wraps the error via
  `Wrapf` before returning on the failure path. The variadic arguments after
  `expr` are forwarded to `Wrapf` (format string + args).
- Same `__LINE__`-based unique naming and scope-leaking trade-offs as the
  non-wrapping variant.
- The error is moved out of the `Result` via the rvalue `error()` accessor
  before passing to `Wrapf`, avoiding a refcount bump.

### `ERRORS_TRY(expr)` (in `result.h`, GCC/Clang only)

```cpp
#define ERRORS_TRY(expr)                                                 \
  ({                                                                     \
    auto&& _result_ = (expr);                                            \
    if (!_result_.ok()) return std::move(_result_).error();              \
    std::move(_result_).value();                                         \
  })
```

Design notes:

- Uses a **GCC statement expression** (`({ ... })`), which is a compiler
  extension supported by both GCC and Clang. Guarded behind
  `#if defined(__GNUC__)`. Not available on MSVC.
- The statement expression evaluates to the unwrapped value, so the macro can
  be used inline in expressions: `auto x = ERRORS_TRY(Foo()) + 1;`
- Uses `auto&&` to bind the result by reference, avoiding a copy.
- The `_result_` name is not `__LINE__`-suffixed because the statement
  expression creates its own scope. Multiple uses on the same line are safe.

## Build

```bash
cmake -B build -DCMAKE_CXX_COMPILER=clang++-20 errors
cmake --build build
ctest --test-dir build
```

Requires a C++23 compiler. Tested with Clang 20 and GCC 14.

## Testing

Tests use Google Test (fetched via CMake `FetchContent`). The test file covers:

| Test | What it verifies |
|---|---|
| `NilBehavior` | Default and `Nil()` construction, bool conversion, message |
| `BasicCreation` | `New` and `Errorf` produce correct messages |
| `Sentinels` | Identity, equality, `Is` matching |
| `Wrapping` | Message chaining, `Is` through wrapped layers |
| `PayloadExtraction` | `NewWithPayload`, `As<T>` extraction, unrelated type returns nullptr |
| `InPlacePayload` | `std::in_place_type` construction path |
| `WrappedPayloads` | Multiple payloads at different chain depths, both accessible via `As` |
| `SSOVerification` | Short and long messages both produce correct output |
| `EmptyMessage` | Empty string error is non-nil with empty message |
| `SingleCharMessage` | Single-character message round-trips correctly |
| `SelfCopyAssignment` | Self-copy-assignment leaves error unchanged |
| `SelfMoveAssignment` | Self-move-assignment leaves error in valid state |
| `MoveFromStateIsNil` | Moved-from error becomes nil |
| `DeepChain` | 100-layer chain: `Is` finds sentinel, `Unwrap` counts depth |
| `CopySharesState` | Copied errors share state, both read identical payloads |
| `COWOnMutableAs` | Non-const `As<T>` mutation does not affect the original (COW) |
| `WhatReturnsSingleLayer` | `what()` returns only the outermost message layer |
| `UnwrapWalksChain` | `Unwrap()` traverses each layer in order |
| `NilWhatAndUnwrap` | Nil error: `what()` is empty, `Unwrap()` is nullptr |
| `SentinelWhatAndUnwrap` | Sentinel: `what()` returns message, `Unwrap()` is nullptr |
| `TwoNewErrorsDoNotMatch` | Two `New("same")` errors are not `Is`-equal |
| `IsWithNilTarget` | `Is(err, nil)` returns false for non-nil errors |
| `AsOnNilReturnsNullptr` | `As<T>` on nil returns nullptr (const and non-const) |
| `ResultTest.SuccessPath` | `Result<int>` success: `ok()`, `value()`, `operator*` |
| `ResultTest.FailurePath` | `Result<int>` failure: `!ok()`, `error().message()` |
| `ResultTest.MoveSemantics` | `Result<string>` move construction preserves value |
| `ResultTest.CopySemantics` | `Result<int>` copy preserves both source and dest |
| `ResultTest.OperatorArrow` | `operator->` accesses members of the held value |
| `ResultTest.SentinelErrorInResult` | Sentinel error stored in `Result<int>` |
| `ResultTest.MoveValueOut` | `std::move(result).value()` extracts the value |
| `SerializationTest.IsSerializable_*` (8 tests) | `IsSerializable` returns correct result for nil, sentinel, dynamic, WireSerializable payload, non-serializable payload, and chains |
| `SerializationTest.DebugString_*` (5 tests) | `DebugString` output for nil, simple, payload (with type+debug info), chain, and non-serializable payload (falls back to message) |
| `SerializationTest.SerializeDeserialize_*` (5 tests) | Round-trip: nil, simple, with payload (reconstructed via `ParseFromString`), chain, multi-layer payloads |
| `SerializationTest.Deserialize_*` (2 tests) | Empty and truncated input handled gracefully |
| `SerializationTest.RoundTrip_DebugStringPreserved` | `DebugString` works on deserialized errors (payload shows as `SerializedPayload`) |
| `ResultVoidTest.*` (9 tests) | Default construction is success, construction from Error is failure, `ok()`/`operator bool`, error access (const/mutable/move), move and copy semantics, sentinel identity, `sizeof == 8` |
| `MacroReturnIfErrorTest.*` (4 tests) | Success path (Error-returning and Result\<void\>-returning callers), failure path from Error-returning function |
| `MacroAssignOrReturnTest.*` (4 tests) | Success path (assigns value), failure path (returns error), existing variable, sentinel identity preserved through return |
| `MacroTryTest.*` (3 tests, GCC/Clang) | Success path (evaluates to value), failure path (returns error), inline usage in expressions |
| `MacroReturnIfErrorWrapfTest.*` (3 tests) | Success path (no wrapping), failure path (wraps with formatted context), works with `Result<void>` return |
| `MacroAssignOrReturnWrapfTest.*` (3 tests) | Success path (assigns value), failure path (wraps error with formatted context), existing variable |

A `static_assert(sizeof(errors::Error) == 8)` at the top of the test file
locks in the 8-byte representation. If the layout changes, the build fails
immediately.

## Adding a new ErrorImpl subclass

1. Define your class in `error_internal.h`, inheriting from `ErrorImpl` (or
   `DynamicError` if it needs a message and inner chain).
2. Override at minimum `message_view()`. Override `clone()` if the error can be
   copied (return `new YourType(*this)`). Override `get_payload()` if it carries
   typed data. Override `matches()` if it needs custom `Is()` semantics.
   Override `is_serializable()` if the error can be serialized (return `true`
   for dynamic errors, `false` for sentinels or errors with non-serializable
   state). If the subclass carries a payload that should be serializable,
   also override `serialize_payload()`, `payload_type_url()`, and
   `payload_debug_string()`.
3. Add a factory function in `error.h` (declared as a `friend` of `Error` so it
   can call the private `Error(ErrorImpl*)` constructor).
4. Add tests.

## Common pitfalls

- **Do not include `error_internal.h` directly.** It will fail with a
  `#error`. Always include `error.h`.
- **Sentinel errors are not owned.** `Error`'s destructor only calls `delete`
  on dynamic (even, non-zero) pointers. Sentinels (odd) are never freed.
  If you add a new encoding kind, make sure the destructor and copy/move
  operations handle it correctly.
- **`ErrorImpl` alignment must be >= 2.** The tagged pointer steals the low
  bit. A `static_assert` in `error.cc` enforces this. Do not add an
  `ErrorImpl` subclass with `alignas(1)`.
- **Rule-of-5 in `DynamicError`.** The SSO union requires manual copy/move.
  If you add members to `DynamicError`, update all five special member
  functions.
- **Non-const `As<T>` triggers COW.** If multiple `Error` handles share the
  same impl (via copy), calling non-const `As<T>()` on one of them will clone
  the outermost layer. This is intentional -- it preserves value semantics --
  but avoid gratuitous non-const `As` calls in hot paths if the payload is
  only being read.
- **Thread safety is per-object, not per-value.** Multiple threads may read
  the same `Error` concurrently (the refcount is atomic). Concurrent
  read+write to the *same* `Error` object is a data race, just like
  `std::string`.
- **`type_id<T>()` does not work across shared library boundaries.**
  Each instantiation of `type_id<T>()` produces a unique address within a
  single link unit. If a payload is created in one `.so` and `As<T>()` is
  called from a different `.so`, the addresses will differ and the match
  will silently fail. This is a fundamental limitation of the static-address
  type ID technique. If cross-`.so` payload extraction is needed, consider a
  string-based type registry instead.
- **Move-only types cannot be used as error payloads.** `Error` uses
  copy-on-write semantics, which requires cloning `DetailedError<T>` via its
  copy constructor. A `static_assert` in `DetailedError<T>::clone()` will
  fire at compile time if `T` is not copy-constructible, providing a clear
  diagnostic.
- **`ERRORS_RETURN_IF_ERROR` only accepts `Error` expressions.** Do not pass
  a `Result<void>` to it. `Error::operator bool` returns `true` for non-nil
  (error), while `Result<void>::operator bool` returns `true` for success.
  Passing a `Result<void>` would invert the early-return condition.
- **`ERRORS_ASSIGN_OR_RETURN` cannot be used twice on the same line.** The
  `__LINE__`-based unique variable name would collide. This is a known
  limitation of the technique (shared with absl's `ASSIGN_OR_RETURN`).
- **`ERRORS_ASSIGN_OR_RETURN` leaks a temporary into the enclosing scope.**
  The generated variable (`errors_result_<LINE>`) is visible after the macro.
  Avoid naming your own variables with the `errors_result_` prefix.
- **`ERRORS_TRY` is not portable.** It uses GCC statement expressions, which
  are not part of the C++ standard. It is only available under `__GNUC__`
  (GCC and Clang). Code that must compile on MSVC should use
  `ERRORS_ASSIGN_OR_RETURN` instead.

## Why not...

### `std::expected<T, E>`?

`std::expected` is the right tool when `T` and `E` are both known locally. It
falls apart at API boundaries: if module A returns `expected<T, AError>` and
module B returns `expected<U, BError>`, the caller must handle both or wrap
them into a third variant. With `errors::Error`, every module returns the same
type, and context is added by wrapping. No variant proliferation.

### Exceptions?

Exceptions impose hidden control flow. Any function might throw, so reasoning
about resource cleanup requires RAII everywhere and careful analysis of
exception-safety guarantees. Exceptions are also slow to throw (~10-100x
compared to a normal return) and make performance unpredictable. Value-based
error returns are explicit, composable, and have predictable performance.

### `std::error_code` / `std::error_condition`?

The `<system_error>` facility has no wrapping, no context strings, no typed
payloads, and an awkward category system. It was designed for OS-level error
codes, not application-level error handling with rich context.

### Abseil `absl::Status`?

`absl::Status` is a reasonable design but couples error identity to a fixed
enum of canonical codes (`kNotFound`, `kPermissionDenied`, ...). Application-
specific sentinel errors require payloads or string matching. This library
allows arbitrary sentinel identities defined at the point of use, with no
central enum.
