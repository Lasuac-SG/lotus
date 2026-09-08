add_library(CanaryInterleavedDyckSPDS STATIC Solver.cpp)
target_include_directories(CanaryInterleavedDyckSPDS PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include)
target_compile_features(CanaryInterleavedDyckSPDS PUBLIC cxx_std_17)
target_link_libraries(CanaryInterleavedDyckSPDS PUBLIC CanaryInterleavedDyckCore)
