add_library(CanaryInterleavedDyckMCFL STATIC
    Grammar.cpp
    Graph.cpp
    InterleavedDyck.cpp
    Solver.cpp
)

target_link_libraries(CanaryInterleavedDyckMCFL PUBLIC CanaryInterleavedDyckCore)
