# Contributing to framelink

Thanks for your interest. framelink is small on purpose, so the rules below are strict. They are what lets it run on an 8 KB-RAM microcontroller and still be trusted on a hostile wire.

## Hard rules

Every change must keep these true. CI enforces most of them.

1. **C99 only.** No compiler extensions in `framelink/`.
2. **Two files.** The library is `framelink/framelink.h` and `framelink/framelink.c`. Nothing else is needed to use it.
3. **No dynamic allocation.** No `malloc`, `calloc`, `realloc` or `free`, ever. All memory belongs to the caller.
4. **No dependencies.** The library includes only `<stddef.h>` and `<stdint.h>`.
5. **No global mutable state.** Every function works on caller-owned structures.
6. **No hidden time or threads.** The caller passes the current time in milliseconds. The library never sleeps or locks.
7. **The spec is normative.** Wire behaviour follows [docs/SPEC.md](docs/SPEC.md). A change to the wire format changes the spec, the test vectors and the version in the same pull request.
8. **Warnings are errors.** The library builds cleanly with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow` on GCC and Clang, and `/W4` on MSVC.

## Building and testing

```sh
cmake -B build -DFL_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Before opening a pull request, also check the other build variants:

```sh
cmake -B build-table -DFL_CRC_TABLE=ON && cmake --build build-table && ctest --test-dir build-table
cmake -B build-small -DFL_MAX_PAYLOAD=64 && cmake --build build-small && ctest --test-dir build-small
```

Fuzzing instructions are in [fuzz/README.md](fuzz/README.md).

## Test vectors

`tests/vectors/frames.json` is the single source of truth for wire-format examples. The C tests and the Python host implementation both use it.

After editing it, regenerate the C header and the fuzz seeds:

```sh
python tests/vectors/gen_vectors.py
python fuzz/make_corpus.py
```

The generator checks every vector against an independent Python encoder and refuses to write a header if any vector is wrong.

## Tests for bug fixes

A bug fix comes with a test that fails without the fix. For decoder bugs found by fuzzing, also add the minimised input as a named seed in `fuzz/make_corpus.py`.

## Commits and pull requests

- One logical change per commit, with a message in the imperative mood: "Add NAK handling", not "Added NAK handling".
- Work on a short-lived branch named after the change, such as `feat/link-layer` or `fix/rescan-overflow`, and open a pull request against `main`.
- CI must be green before merging.

## Code style

- 4-space indentation, braces on the same line except for function definitions.
- Public names start with `fl_` and macros with `FL_`.
- Explicit casts where integer widths narrow, so `-Wconversion` stays quiet without hiding real problems.
- Comments explain why, not what. Reference the spec section where a rule comes from.
