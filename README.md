# ndof-patterns

NDoF design patterns

Part of the ndof framework family. Requires C++23 (GCC 13+, Clang 17+, MSVC 19.36+).

Licensed under the [Apache License 2.0](LICENSE).

## Development

The canonical environment is the ndof dev container, defined in
[ndof-infra](https://github.com/ndof-opensource/ndof-infra) — full onboarding guide:
[docs/onboarding.md](https://github.com/ndof-opensource/ndof-infra/blob/main/docs/onboarding.md).

Prerequisites: Docker (Docker Desktop or OrbStack) and a devcontainer-capable
editor (VS Code, Zed, JetBrains) or the
[devcontainer CLI](https://github.com/devcontainers/cli). Nothing else is
installed on your host.

1. Clone this repository and open the folder in your editor.
2. Accept the "reopen in container" prompt. The first open downloads the
   pinned dev image (one-time; cached for all ndof repos thereafter).
3. In the container terminal:

```sh
scripts/build.sh debug   # resolve deps + configure + build + test
```

The script prints each command it runs; the raw sequence it wraps is:

```sh
conan install . --build=missing -pr:a /opt/conan/profiles/linux-gcc14 -s build_type=Debug
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release`, `asan`, `tsan` (sanitizer presets reuse the Debug
conan install). Switch compilers with `scripts/build.sh --profile linux-clang19`
(the script cleans `build/` automatically when the profile changes). CI runs
the same presets on Linux (GCC + Clang), macOS, and Windows, plus clang-format
and clang-tidy gates.

## Consuming

As a Conan package:

```
requires = "ndof-patterns/0.1.0"
```

Or as a plain CMake package (no package manager required):

```cmake
find_package(ndof-patterns REQUIRED)
target_link_libraries(your_target PRIVATE ndof::patterns)
```
