add_library(CanaryInterleavedDyckLCL STATIC
    Solver.cpp)

target_link_libraries(CanaryInterleavedDyckLCL
    PUBLIC
        CanaryInterleavedDyckCore)

target_compile_features(CanaryInterleavedDyckLCL PUBLIC cxx_std_17)
