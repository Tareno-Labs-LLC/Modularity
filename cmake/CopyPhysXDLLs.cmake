if(NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
    message(FATAL_ERROR "CopyPhysXDLLs.cmake requires SRC_DIR and DST_DIR.")
endif()

if(NOT EXISTS "${SRC_DIR}")
    return()
endif()

file(MAKE_DIRECTORY "${DST_DIR}")
file(GLOB_RECURSE PHYSX_DLLS RELATIVE "${SRC_DIR}" "${SRC_DIR}/*.dll")

foreach(dll ${PHYSX_DLLS})
    set(src "${SRC_DIR}/${dll}")
    if(EXISTS "${src}")
        # Skip the empty freeglut.dll/freeglutd.dll placeholders CMakeLists.txt writes
        # only to satisfy PhysX's configure; a 0-byte DLL is never loadable.
        file(SIZE "${src}" _physx_dll_size)
        if(_physx_dll_size EQUAL 0)
            continue()
        endif()
        file(COPY "${src}" DESTINATION "${DST_DIR}")
    endif()
endforeach()
