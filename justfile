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
    lcov -c -d build-coverage --rc branch_coverage=1 \
        --ignore-errors mismatch,unused \
        --include "*/include/*" --include "*/src/*" \
        --exclude "*/_deps/*" \
        -o build-coverage/coverage.info
    genhtml build-coverage/coverage.info --branch-coverage -o build-coverage/coverage_html
    open build-coverage/coverage_html/index.html

clean:
    rm -rf build/*
