add_library(CanaryInterleavedDyckStagedBounds STATIC
    Solver.cpp
)

target_link_libraries(CanaryInterleavedDyckStagedBounds
    PUBLIC
        CanaryInterleavedDyckCore
        CanaryInterleavedDyckMutualRefinement)