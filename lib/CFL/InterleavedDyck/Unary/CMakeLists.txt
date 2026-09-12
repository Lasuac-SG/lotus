add_library(CanaryInterleavedDyckUnary STATIC
    Adaptive.cpp
    FixedCounter.cpp
)

target_link_libraries(CanaryInterleavedDyckUnary
    PUBLIC
        CanaryInterleavedDyckCore)