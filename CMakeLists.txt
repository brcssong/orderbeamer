cmake_minimum_required(VERSION 4.2)
project(orderbeamer)

set(CMAKE_CXX_STANDARD 20)

include_directories(.)

add_executable(orderbeamer
        gateway/gateway.h
        engine/engine.cpp
        engine/statics.h
        generics/Error.h
        generics/FD.h
        memory/Allocator.h
        memory/Buffer.h
        matcher/matcher.cpp
        matcher/matcher.h
        memory/PriceLevelArena.h
        memory/OrderArena.h
        memory/Vector.h)

# Benchmark + correctness harness for the matching core (portable: no gateway/epoll).
# Build under the Docker/Linux toolchain for pinned numbers. -O3 forced so it is
# always optimized, regardless of the active build profile.
add_executable(bench tests/bench.cpp)
target_compile_options(bench PRIVATE -O3)

# Differential stress test: chaotic op-stream vs a std::map reference engine.
add_executable(stress tests/stress.cpp)
target_compile_options(stress PRIVATE -O3)
