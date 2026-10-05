# Cross-compile f19.exe for Windows (needs mingw-w64-gcc; the first build
# downloads SDL3, the OpenXR loader and zlib). Output: build/windows/f19.exe,
# self-contained.
set -e
cmake -S native -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/native/cmake/mingw-w64.cmake"
ninja -C build/windows f19 f19trace
ls -l build/windows/f19.exe
