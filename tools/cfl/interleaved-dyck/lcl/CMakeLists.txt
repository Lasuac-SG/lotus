add_executable(lotus-cfl-interleaved-dyck-lcl
    lotus-cfl-interleaved-dyck-lcl.cpp)

target_link_libraries(lotus-cfl-interleaved-dyck-lcl
    PRIVATE
        CanaryInterleavedDyckLCL)
