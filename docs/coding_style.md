# Coding Style

This document pins down the binding conventions for C++ code in
`chesserazade`. Everything here is a restatement of
[HANDOFF.md §3](../HANDOFF.md) plus the few decisions that have
been made as the code grew.

## Language and toolchain

- **C++23** is the baseline. C++20 is the lowest accepted fallback,
  and only if a later refactor needs to degrade; today we assume 23.
- Compilers supported in CI: **GCC ≥ 13**, **Clang ≥ 17**. The
  reference build machine runs GCC 15 / Clang 20.
- **CMake ≥ 3.25**, driven via `CMakePresets.json`
  (`debug`, `release`, `release-with-asserts`).
- Warnings are errors. The shared `chesserazade_warnings` interface
  target carries: `-Wall -Wextra -Wpedantic -Werror -Wshadow
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Woverloaded-virtual -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion -Wformat=2`.

## Naming

| Category  | Style              | Example                     |
|-----------|--------------------|-----------------------------|
| Type      | `PascalCase`       | `Board8x8Mailbox`           |
| Function  | `snake_case`       | `serialize_fen`, `piece_at` |
| Variable  | `snake_case`       | `side_to_move`, `ep_square` |
| Constant  | `SCREAMING_SNAKE`  | `NUM_SQUARES`, `STARTING_POSITION_FEN` |
| Enum kind | `PascalCase`       | `enum class Color { White, Black };` |
| File      | `snake_case.hpp`/`.cpp` | `board8x8_mailbox.hpp` |

## File layout

- Public headers live under `include/chesserazade/`.
- Private headers live alongside their implementation in `src/`.
- **One public type per header** where practical.
- Every public header begins with a `///` doc-block describing the
  module's purpose, its invariants, and a link to the Chess
  Programming Wiki page when one exists.
- Source files aim for **≤ 400 lines**. When a file grows past that,
  split it by concept — not by line count.

## Comments

This project deliberately **overrides the usual "no comments"
default**. A reader coming to a module for the first time should be
able to orient themselves from the header's doc-block and the
comments above non-trivial routines. Specifically:

- **Yes, comment.** Add a doc-block to every public function, type,
  and header. Explain invariants, coordinate conventions,
  and — when implementing a classical algorithm — what it is and
  why it works (e.g. what alpha-beta prunes, why Zobrist collisions
  are acceptable).
- **Do not comment the obvious.** `// increment i` is noise. Name
  the loop variable well and move on.
- **Reference the Chess Programming Wiki** from headers that
  implement a classical technique.

## Language features

- **Templates:** only where they arise naturally from DRY. Do *not*
  template-ify `Board`, `MoveGenerator`, `Search`. Polymorphism uses
  abstract base classes and `virtual`.
- **Lambdas:** avoided. A named local function or free helper is
  easier to step through in a debugger.
- **Macros:** none, except `#pragma once` style include guards.
- **`using namespace std;`:** never at file scope.
- **Error handling:**
  - `std::expected<T, Error>` for recoverable errors
    (parse failures, malformed FEN, illegal move input). See
    `FenError` in `chesserazade/fen.hpp` for the model.
  - `assert` for invariants that indicate engine bugs.
  - Exceptions only at the CLI / front-end boundary.

## Formatting

Driven by `.clang-format` at the repo root. Highlights:

- 4-space indent, no tabs, 100-column soft limit.
- Braces on the same line as the control statement.
- Pointer on the type: `int* p`, not `int *p`.
- Headers regrouped with `<chesserazade/...>` first, then project
  quoted includes, then system `<...>` headers.

If you ever need to deviate from `.clang-format`, write one sentence
above the block saying *why*.

## Testing

- Catch2 v3 is pulled via `FetchContent`. No other test dependency.
- Tests live under `tests/`, one `test_<module>.cpp` per module.
- **Deterministic.** No time-limited tests in CI; use depth- or
  node-limits when a search is involved.
- Tests may reach into `src/` for private headers via the
  `src/` include path already configured for the test binary.

## Commit messages

- Imperative mood, scoped (`fen: accept non-canonical castling
  order`).
- Body explains *why* when non-obvious. Current-state wording, not
  "will do X".
- **No `--no-verify`**, no hook-skipping. If a hook fails, fix the
  root cause.

## Bilingual documentation

Design documents carry a Polish twin. The canonical English version
lives at the intended location
(e.g. `HANDOFF.md`, `docs/coding_style.md`); the Polish translation
lives alongside with an `_pl` suffix
(`docs/HANDOFF_pl.md`, `docs/coding_style_pl.md`). Both are kept
in sync on every edit.
