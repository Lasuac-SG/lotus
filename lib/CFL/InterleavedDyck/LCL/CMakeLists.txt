add_library(CanaryInterleavedDyckLCL STATIC
    Solver.cpp)

target_link_libraries(CanaryInterleavedDyckLCL
    PUBLIC
        CanaryInterleavedDyckCore)