#!/usr/bin/env bash
# One-command build driver: resolve dependencies, configure, build, test.
#
# usage: scripts/build.sh [debug|release|asan|tsan] [--clean] [--profile <name-or-path>]
#
#   debug (default), release   normal builds
#   asan, tsan                 sanitizer builds (reuse the Debug dependency install)
#   --clean                    delete build/ first
#   --profile <p>              Conan profile: a name under /opt/conan/profiles
#                              (inside the dev container) or a path. Outside the
#                              container, defaults to Conan's detected profile.
#
# The script prints every command it runs — it is a convenience, not a layer
# of magic. CI runs the same underlying commands explicitly; if this script
# disappeared, only convenience would be lost. On native Windows, use the raw
# commands from README.md / .github/workflows/ci.yml.
set -euo pipefail

run() {
    echo "+ $*" >&2
    "$@"
}

PRESET=debug
CLEAN=0
PROFILE=""

while [[ $# -gt 0 ]]; do
    case $1 in
        debug | release | asan | tsan) PRESET=$1 ;;
        --clean) CLEAN=1 ;;
        --profile)
            PROFILE=$2
            shift
            ;;
        *)
            echo "unknown argument: $1 (see header of $0)" >&2
            exit 1
            ;;
    esac
    shift
done

# Sanitizer presets are compiled against the Debug dependency install
# (their toolchainFile in CMakePresets.json points at build/Debug/generators).
case $PRESET in
    release) BUILD_TYPE=Release ;;
    *) BUILD_TYPE=Debug ;;
esac

cd "$(dirname "$0")/.."

# Fail early, with the real cause, if the checkout is not writable. On Linux
# hosts a bind mount preserves numeric uids: if the container user's uid
# differs from the checkout owner's, every write fails (the first is usually
# deep inside Conan). Devcontainer tools prevent this by remapping the
# container user's uid to the host user's (updateRemoteUserUID); a raw
# `docker run` or a buggy editor integration does not.
if [[ ! -w . ]]; then
    echo "error: $PWD is not writable by $(id -un 2>/dev/null || echo unknown) (uid $(id -u))" >&2
    echo "This is usually a bind-mount uid mismatch; see \"Common pitfalls\" in" >&2
    echo "the ndof-infra onboarding guide (linked from this repo's README):" >&2
    echo "https://github.com/ndof-opensource/ndof-infra/blob/main/docs/onboarding.md" >&2
    echo "Do not 'fix' it with chmod -R 777: that marks every tracked file as" >&2
    echo "modified (mode changes) in git." >&2
    exit 1
fi

# Resolve the profile: team profiles live in the dev image; on a bare host
# fall back to Conan's auto-detected profile pinned to C++23.
PROFILE_ARGS=()
if [[ -n $PROFILE ]]; then
    if [[ -f /opt/conan/profiles/$PROFILE ]]; then
        PROFILE=/opt/conan/profiles/$PROFILE
    fi
    PROFILE_ARGS=(-pr:a "$PROFILE")
elif [[ -d /opt/conan/profiles ]]; then
    PROFILE=/opt/conan/profiles/linux-gcc14
    PROFILE_ARGS=(-pr:a "$PROFILE")
else
    PROFILE=default
    if [[ ! -f "$(conan config home)/profiles/default" ]]; then
        run conan profile detect
    fi
    PROFILE_ARGS=(-s compiler.cppstd=23)
fi

# Switching compiler profiles requires a clean tree: CMake caches the compiler
# and silently ignores a changed toolchain in an existing build directory.
if [[ $CLEAN -eq 1 ]]; then
    run rm -rf build
elif [[ -f build/.last-profile && "$(cat build/.last-profile)" != "$PROFILE" ]]; then
    echo "profile changed ($(cat build/.last-profile) -> $PROFILE): cleaning build/" >&2
    run rm -rf build
fi

# A build tree also remembers the absolute path it was created at. Using the
# same checkout from different mount points (plain `docker run` at /work, a
# devcontainer at /workspaces/<name>, the bare host) breaks CMake's cached
# paths; detect the move and clean, as with a profile switch.
for cache in build/*/CMakeCache.txt; do
    [[ -f $cache ]] || continue
    cached_src=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")
    if [[ -n $cached_src && $cached_src != "$PWD" ]]; then
        echo "build tree was created at $cached_src (now $PWD): cleaning build/" >&2
        run rm -rf build
        break
    fi
done

# Dependencies on other ndof libraries (declared with `requires` in
# conanfile.py) are downloaded from the public ndof Conan remote, so make
# sure Conan knows about it. Anonymous, idempotent, and harmless for a
# library that depends on no other ndof library. Details:
# ndof-infra/docs/releasing.md.
if ! conan remote list | grep -q "^ndof-public:"; then
    run conan remote add ndof-public https://conan.cloudsmith.io/ndof-opensource/packages/
fi

run conan install . --build=missing "${PROFILE_ARGS[@]}" -s build_type=$BUILD_TYPE
mkdir -p build
echo "$PROFILE" > build/.last-profile

run cmake --preset "$PRESET"
run cmake --build --preset "$PRESET"
run ctest --preset "$PRESET"
