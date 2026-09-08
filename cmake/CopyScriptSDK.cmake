cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SRC_ROOT OR NOT DEFINED DST_ROOT)
    message(FATAL_ERROR "CopyScriptSDK.cmake requires SRC_ROOT and DST_ROOT.")
endif()

if(NOT DEFINED BUILD_ROOT)
    set(BUILD_ROOT "")
endif()

function(copy_regular_files base_dir rel_dir)
    set(src_dir "${base_dir}/${rel_dir}")
    if(NOT EXISTS "${src_dir}")
        return()
    endif()

    file(GLOB_RECURSE files LIST_DIRECTORIES false "${src_dir}/*")
    foreach(src_file IN LISTS files)
        file(RELATIVE_PATH rel_path "${base_dir}" "${src_file}")
        get_filename_component(rel_parent "${rel_path}" DIRECTORY)
        file(MAKE_DIRECTORY "${DST_ROOT}/${rel_parent}")
        file(COPY_FILE "${src_file}" "${DST_ROOT}/${rel_path}" ONLY_IF_DIFFERENT)
    endforeach()
endfunction()

function(copy_headers_filtered rel_dir)
    set(src_dir "${SRC_ROOT}/${rel_dir}")
    if(NOT EXISTS "${src_dir}")
        return()
    endif()

    file(GLOB_RECURSE headers LIST_DIRECTORIES false
        "${src_dir}/*.h"
        "${src_dir}/*.hpp"
        "${src_dir}/*.inl"
    )

    foreach(src_file IN LISTS headers)
        string(REPLACE "\\" "/" normalized "${src_file}")
        if(normalized MATCHES "/src/ThirdParty/")
            continue()
        endif()
        file(RELATIVE_PATH rel_path "${SRC_ROOT}" "${src_file}")
        get_filename_component(rel_parent "${rel_path}" DIRECTORY)
        file(MAKE_DIRECTORY "${DST_ROOT}/${rel_parent}")
        file(COPY_FILE "${src_file}" "${DST_ROOT}/${rel_path}" ONLY_IF_DIFFERENT)
    endforeach()
endfunction()

file(MAKE_DIRECTORY "${DST_ROOT}")

copy_headers_filtered("src")
copy_regular_files("${SRC_ROOT}" "include")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/glad")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/glm")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/glfw/include")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/ModuGUI")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/ImGuizmo")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/ImGuiColorTextEdit")
copy_regular_files("${SRC_ROOT}" "src/ThirdParty/assimp/include")

if(BUILD_ROOT AND EXISTS "${BUILD_ROOT}/src/ThirdParty/assimp/include")
    copy_regular_files("${BUILD_ROOT}" "src/ThirdParty/assimp/include")
endif()
