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
        file(COPY "${src}" DESTINATION "${DST_DIR}")
    endif()
endforeach()
