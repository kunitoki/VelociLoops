default:
    @just -l

prepare:
    mkdir -p build

generate: prepare
    cmake -G Xcode -B build .
    -open build/VelociLoops.xcodeproj

clean:
    rm -rf build/*
