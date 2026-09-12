#!/usr/bin/env bash
# Build an isolated, pinned runtime; never replace system libraries.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
work_root=${1:-"$repo_root/build_tsan_runtime"}
source_dir=${MINI_LLVM_SOURCE_DIR:-"$work_root/source"}
build_dir=${MINI_TSAN_RUNTIME_BUILD:-"$work_root/build"}
prefix=${MINI_TSAN_RUNTIME_PREFIX:-"$work_root/install"}
source_dir=$(realpath -m -- "$source_dir")
build_dir=$(realpath -m -- "$build_dir")
prefix=$(realpath -m -- "$prefix")
llvm_tag=llvmorg-18.1.3
llvm_commit=c13b7485b87909fcf739f62cfa382b55407433c0

if [[ ! -d "$source_dir/.git" ]]; then
    mkdir -p -- "$(dirname -- "$source_dir")"
    git clone --depth 1 --filter=blob:none --sparse --branch "$llvm_tag" \
        https://github.com/llvm/llvm-project.git "$source_dir"
    git -C "$source_dir" sparse-checkout set cmake runtimes libcxx libcxxabi libunwind llvm/cmake
fi
if [[ $(git -C "$source_dir" rev-parse HEAD) != "$llvm_commit" ]] ||
   ! git -C "$source_dir" diff --quiet HEAD; then
    printf 'LLVM source must be the clean pinned revision %s\n' "$llvm_commit" >&2
    exit 1
fi

cmake -G Ninja -S "$source_dir/runtimes" -B "$build_dir" \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    '-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind' -DLLVM_USE_SANITIZER=Thread \
    -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_ENABLE_STATIC=OFF \
    -DLIBCXXABI_ENABLE_STATIC=OFF -DLIBUNWIND_ENABLE_STATIC=OFF \
    "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_INSTALL_RPATH=$prefix/lib"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
    --target install-cxx install-cxxabi install-unwind

flags=(-std=c++23 -O0 -g -fsanitize=thread -pthread -stdlib=libc++
       -fexperimental-library -nostdinc++ -isystem "$prefix/include/c++/v1"
       -L "$prefix/lib" "-Wl,-rpath,$prefix/lib")
probe="$repo_root/tests/toolchain/shared_weak_probe.cpp"
clang++-18 "${flags[@]}" "$probe" -o "$build_dir/shared_weak_probe"
ldd "$build_dir/shared_weak_probe" > "$build_dir/runtime-links.log"
for library in libc++.so.1 libc++abi.so.1 libunwind.so.1; do
    resolved=$(awk -v name="$library" '$1 == name { print $3 }' "$build_dir/runtime-links.log")
    case "$resolved" in
        "$prefix/lib/"*) ;;
        *) printf 'Unexpected runtime for %s: %s\n' "$library" "$resolved" >&2; exit 1 ;;
    esac
done
"$build_dir/shared_weak_probe"
clang++-18 "${flags[@]}" -DMINI_TSAN_NEGATIVE_CONTROL "$probe" -o "$build_dir/negative_probe"
set +e
"$build_dir/negative_probe" > "$build_dir/negative-control.log" 2>&1
negative_status=$?
set -e
if [[ "$negative_status" -ne 66 ]] ||
   ! grep -q 'WARNING: ThreadSanitizer: data race' "$build_dir/negative-control.log"; then
    cat "$build_dir/negative-control.log"
    printf 'Expected a TSan data-race report and status 66, got status %s\n' "$negative_status" >&2
    exit 1
fi
printf '%s\n' "$prefix" > "$build_dir/runtime-prefix.txt"
printf 'Verified instrumented runtime: %s\n' "$prefix"
