if(NOT TARGET CanaryInterleavedDyckSPDS)
    message(FATAL_ERROR "AffineSPDS requires the SPDS engine. Add SPDS before AffineSPDS.")
endif()
add_library(CanaryInterleavedDyckAffineSPDS STATIC
    Matrix.cpp
    Affine.cpp
    Observer.cpp
    Solver.cpp)
target_compile_features(CanaryInterleavedDyckAffineSPDS PUBLIC cxx_std_17)
target_include_directories(CanaryInterleavedDyckAffineSPDS PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include)
target_link_libraries(CanaryInterleavedDyckAffineSPDS PUBLIC CanaryInterleavedDyckSPDS)
