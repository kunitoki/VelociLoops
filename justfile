default:
    @just -l

prepare:
    mkdir -p build

generate: prepare
    cmake -G Xcode -B build .
    -open build/VelociLoops.xcodeproj

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

bump:
    perl -0pi -e 's/x=(\d+)/"x=" . ($1 + 1)/ge' README.md

clean:
    rm -rf build/*
