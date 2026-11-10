#!/usr/bin/env sh
set -eu

BUILD_JOBS=${BUILD_JOBS:-2}

# Cross compiles for the pi. Run it in the docker container -> build/mini-drivebrain
conan profile detect --force
conan install . --build=missing --profile:host=rpi_profile -s:b compiler.cppstd=17 -of=build -c tools.build:jobs="$BUILD_JOBS"

cd build
. ./conanbuild.sh
cmake .. -DDRIVEBRAIN_STATIC=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
cmake --build . --parallel "$BUILD_JOBS"
