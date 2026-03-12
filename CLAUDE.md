@AGENTS.md

# Wibo

Windows binary loader for running MSVC cross-compilers on Linux. Used by the dc3-decomp project to run the Xbox 360 MSVC compiler.

## Building

```bash
# Debug (fast iteration)
cmake -B build && cmake --build build

# Release (use after changes settle — half the binary size, optimized)
cmake -B build/release -DCMAKE_BUILD_TYPE=Release -DWIBO_ENABLE_LTO=OFF && cmake --build build/release
```

Note: LTO (`-DWIBO_ENABLE_LTO=ON` or `Release` default) currently causes a SIGSEGV at runtime. Use `-DWIBO_ENABLE_LTO=OFF` until that's resolved.

The dc3-decomp build system references `build/release/wibo`. After building debug during development, copy it over or do a release build before testing with ninja.

## Key Features for dc3-decomp

- `WIBO_FS_CACHE=1` — caches filesystem lookups for faster compilation
- `WIBO_REWRITE_SHOWINCLUDES=1` — rewrites MSVC `/showIncludes` output paths for ninja `deps = msvc` header tracking
- `WIBO_PATH_MAP` — maps Windows paths to Linux paths (e.g. `e:/build/src/=~/decomp/src/`)
- Path case normalization — fixes MSVC's lowercased paths to match case-sensitive Linux filesystem
