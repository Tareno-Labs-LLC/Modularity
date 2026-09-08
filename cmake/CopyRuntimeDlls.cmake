if(NOT DEFINED DST_DIR)
    message(FATAL_ERROR "CopyRuntimeDlls.cmake requires DST_DIR.")
endif()

if(NOT DEFINED RUNTIME_DLLS OR RUNTIME_DLLS STREQUAL "")
    set(RUNTIME_DLLS "")
endif()

foreach(runtime_dll IN LISTS RUNTIME_DLLS)
    if(runtime_dll STREQUAL "")
        continue()
    endif()

    if(EXISTS "${runtime_dll}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime_dll}" "${DST_DIR}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endforeach()

if(DEFINED RUNTIME_DLL_DIRS AND NOT RUNTIME_DLL_DIRS STREQUAL "")
    foreach(runtime_dll_dir IN LISTS RUNTIME_DLL_DIRS)
        if(runtime_dll_dir STREQUAL "" OR NOT IS_DIRECTORY "${runtime_dll_dir}")
            continue()
        endif()

        file(GLOB runtime_dir_dlls "${runtime_dll_dir}/*.dll")
        foreach(runtime_dir_dll IN LISTS runtime_dir_dlls)
            if(EXISTS "${runtime_dir_dll}")
                execute_process(
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime_dir_dll}" "${DST_DIR}"
                    COMMAND_ERROR_IS_FATAL ANY
                )
            endif()
        endforeach()
    endforeach()
endif()
