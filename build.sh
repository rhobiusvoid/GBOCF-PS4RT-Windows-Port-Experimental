#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
command -v cmake >/dev/null
command -v ninja >/dev/null
pkg-config --exists vulkan sdl3 libavcodec libavutil libswscale
# The Lab itself and game-only ports compile without Crypto++.
# Update/DLC-enabled generated ports get the audited extractor source.
if [[ -f gpu/shim/ps4rt_features.h ]] &&
   grep -qx '#define PS4RT_NEEDS_NATIVE_PKG 1' gpu/shim/ps4rt_features.h; then
    bash scripts/build_pkg_native.sh
    bash scripts/build_setup.sh
fi
# Developer diagnostics (gpu/shim/ps4rt_diag.h) are compiled out of player releases.
diagnostics=${PS4RT_DIAGNOSTICS:-}
if [[ -z "$diagnostics" ]]; then
    diagnostics=OFF
    [[ ! -f tools/diagnostics/native/vk_gpu_profile.cpp ]] || diagnostics=ON
fi
[[ ${PS4RT_PORTABLE_BUILD:-0} != 1 ]] || diagnostics=OFF
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS4RT_PGO=off -DPS4RT_LTO="${PS4RT_LTO:-OFF}" \
    -DPS4RT_DIAGNOSTICS="$diagnostics"
ninja -C out/gpu -j "${PS4RT_BUILD_JOBS:-4}" ps4rtgpu
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil libswscale)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 libavcodec libavutil libswscale)"
# The GCC14 portable image has a sysroot distinct from the host toolchain.
# These paths are opt-in; normal local builds retain their old behavior.
portable_libs=()
if [[ ${PS4RT_PORTABLE_BUILD:-0} == 1 ]]; then
    [[ -n ${CXX:-} ]] || { echo 'PS4RT_PORTABLE_BUILD needs CXX' >&2; exit 1; }
    # The GCC14 toolchain's sysroot needs all three dependency search roots.
    # Star Ocean and Valkyria v1.1 both use these paths to resolve zlib,
    # xxhash and X11 transitively while linking the native host executable.
    portable_libs=(-L/usr/lib/x86_64-linux-gnu
        -Wl,-rpath-link,/opt/ps4rt-deps/lib
        -Wl,-rpath-link,/usr/lib/x86_64-linux-gnu
        -Wl,-rpath-link,/lib/x86_64-linux-gnu
        "$("$CXX" -print-file-name=libstdc++.so.6)")
fi
compiler=${CC:-cc}
host_diagnostics=(-DPS4RT_DIAGNOSTICS=0)
if [[ ${diagnostics^^} == ON && -f tools/diagnostics/native/runtime_loading_log.c ]]; then
    host_diagnostics=(-DPS4RT_DIAGNOSTICS=1 tools/diagnostics/native/runtime_loading_log.c)
fi
mkdir -p out/atrac9
for source in third_party/LibAtrac9/C/src/*.c; do
    "$compiler" -std=c99 -O2 -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"
done
ar rcs out/libatrac9.a out/atrac9/*.o
"$compiler" -std=c11 -O2 -Wall -Wextra -Werror -mgeneral-regs-only -c src/isa_emulate.c -o out/isa_emulate.o
"$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -no-pie -DPS4RT_RUNTIME "${includes[@]}" -I. -Isrc \
    "${host_diagnostics[@]}" src/loader.c src/runtime*.c src/cpu_check.c out/isa_emulate.o src/isa_rewrite.c out/libatrac9.a \
    -lm -Lout/gpu -lps4rtgpu -Wl,-rpath,'$ORIGIN/gpu' -rdynamic "${libraries[@]}" "${portable_libs[@]}" -o out/ps4rt
if [[ ${1:-} == --test ]]; then
    ninja -C out/gpu -j "${PS4RT_BUILD_JOBS:-4}" display-sample-gpu-test transition-snapshot-gpu-test \
        cpu-image-writeback-test color-export-test scalar-exec-scope-test image-subresource-test gpu-workarounds-test \
        scene-camera-history-test menu-input-test menu-sounds-test graphics-settings-test achievements-test achievements-ui-test
    if [[ ${diagnostics^^} == ON ]]; then
        ninja -C out/gpu -j "${PS4RT_BUILD_JOBS:-4}" diagnostics-test
    fi
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc \
        tests/test_content.c src/runtime_content.c -o out/content-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc \
        tests/test_mutex.c src/runtime_mutex.c -o out/mutex-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc \
        tests/test_movie_order.c src/runtime_movie_order.c -o out/movie-order-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections -I. -Isrc \
        tests/test_file_read.c src/runtime_file.c -Wl,--gc-sections -Wl,--wrap=read -Wl,--wrap=pread \
        -o out/file-read-test
    ctest --test-dir out/gpu --output-on-failure
    python3 -m unittest discover -s tests -p 'test_*.py' -v
    if [[ -d tools/diagnostics/tests ]]; then
        python3 -m unittest discover -s tools/diagnostics/tests -p 'test_*.py' -v
    fi
fi
