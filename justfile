default:
    @just -l

prepare:
    mkdir -p build

generate: prepare
    cmake -G Xcode -B build .

build: generate
    cmake --build build --parallel $(nproc) -- -quiet

open: generate
    -open build/VelociLoops.xcodeproj

test: build
    ctest --test-dir build -C Debug --output-on-failure --extra-verbose --parallel $(nproc)

run: build
    ./build/demo/Debug/velociloops tests/data/120Stereo.rx2

coverage:
    cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DVELOCILOOPS_ENABLE_COVERAGE=ON
    cmake --build build-coverage --parallel $(nproc)
    ctest --test-dir build-coverage --output-on-failure --extra-verbose
    lcov -c -d build-coverage \
        --ignore-errors mismatch,unused,inconsistent,unsupported \
        --include "$(pwd)/include/*" \
        --include "$(pwd)/src/*" \
        -o build-coverage/coverage.info
    lcov --summary build-coverage/coverage.info --ignore-errors inconsistent,unsupported
    genhtml build-coverage/coverage.info --ignore-errors category,inconsistent,unsupported -o build-coverage/coverage_html
    open build-coverage/coverage_html/index.html

sanitize:
    cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DVELOCILOOPS_ENABLE_SANITIZERS=ON
    cmake --build build-sanitize --parallel $(nproc)
    ctest --test-dir build-sanitize --output-on-failure --extra-verbose

fuzz:
    cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
        -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
        -DVELOCILOOPS_ENABLE_SANITIZERS=ON \
        -DVELOCILOOPS_ENABLE_FUZZING=ON
    cmake --build build-fuzz --parallel $(nproc) --target velociloops_fuzzer
    ASAN_OPTIONS=halt_on_error=1:detect_leaks=0 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        ./build-fuzz/fuzz/velociloops_fuzzer \
        fuzz/data/ -max_total_time=30 -max_len=980128 -rss_limit_mb=512 tests/data/

format:
    clang-format --style=file -i include/*.h src/*.cpp tests/*.cpp

visualize RX="tests/data/120Stereo.rx2": generate
    uv venv --allow-existing
    uv pip install --requirement scripts/requirements.txt
    uv run scripts/visualize_rx2.py {{RX}}

bump:
    perl -0pi -e 's/x=(\d+)/"x=" . ($1 + 1)/ge' README.md

clean:
    rm -rf build/*
