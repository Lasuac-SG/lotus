if(TARGET lotus-cfl-interleaved-dyck-affine-spds)
    foreach(case correlation automatic identity independent backward json crossing mismatch prefix
            explicit source target allpairs empty isolated observer_error unknown mode_error
            limit dimension_limit missing_vertex conflicting_options help roundtrip)
        add_test(NAME affine_spds_cli_${case}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:lotus-cfl-interleaved-dyck-affine-spds>
                -DFIXTURES=${CMAKE_CURRENT_LIST_DIR}
                -DCASE=${case}
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}
                -P ${CMAKE_CURRENT_LIST_DIR}/RunCLI.cmake)
        set_tests_properties(affine_spds_cli_${case} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
