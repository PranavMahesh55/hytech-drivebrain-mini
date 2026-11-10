#!/usr/bin/env sh
set -eu
BUILD_JOBS=${BUILD_JOBS:-2}
# Requires CMake, a C++17 compiler, Conan 2; run from the repository root.
# Creates a separate build tree so it cannot reuse Pi cross-compile artifacts.
conan profile detect --name mini-drivebrain-native --force
conan install . --build=missing -pr:h mini-drivebrain-native -pr:b mini-drivebrain-native -of=build-native -s build_type=Release -c tools.build:jobs="$BUILD_JOBS"
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
cmake --build build-native --parallel "$BUILD_JOBS"
ctest --test-dir build-native --output-on-failure
