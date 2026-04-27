default:
    @just -l

prepare:
    mkdir -p build

generate: prepare
    cmake -G Xcode -B build .

open: generate
    -open build/VelociLoops.xcodeproj

run: generate
    cmake -G Xcode -B build .
    cmake --build build --parallel $(nproc) --target velociloops -- -quiet
    cmake --build build --parallel $(nproc) --target velociloops_static -- -quiet
    cmake --build build --parallel $(nproc) --target velociloops_shared -- -quiet
    ctest --test-dir build -C Debug --output-on-failure
    ./build/demo/Debug/velociloops tests/data/120Stereo.rx2

coverage:
    cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DVELOCILOOPS_COVERAGE=ON
    cmake --build build-coverage --parallel $(nproc)
    ctest --test-dir build-coverage --output-on-failure
    lcov -c -d build-coverage \
        --ignore-errors mismatch,unused,inconsistent,unsupported \
        --include "$(pwd)/include/*" \
        --include "$(pwd)/src/*" \
        -o build-coverage/coverage.info
    lcov --summary build-coverage/coverage.info --ignore-errors inconsistent,unsupported --fail-under-lines 100
    genhtml build-coverage/coverage.info --ignore-errors category,inconsistent,unsupported -o build-coverage/coverage_html
    open build-coverage/coverage_html/index.html

visualize: generate
    uv venv --allow-existing
    uv pip install --requirement scripts/requirements.txt
    uv run scripts/visualize_rx2.py

bump:
    perl -0pi -e 's/x=(\d+)/"x=" . ($1 + 1)/ge' README.md

clean:
    rm -rf build/*
