#!/usr/bin/env bash

set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${project_dir}/build"

echo "==> Konfiguruji CMake"
cmake \
  -S "${project_dir}" \
  -B "${build_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug

echo "==> Performing a clean build"
cmake --build "${build_dir}" --clean-first

echo "==> Running tests"
ctest --test-dir "${build_dir}" --output-on-failure

echo
echo "Build complete. config.json is unchanged."
echo "Run: ${build_dir}/tbot -c ${project_dir}/config.json"

