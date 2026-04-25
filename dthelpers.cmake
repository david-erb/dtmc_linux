# dthelpers.cmake
#
# --------------------------------------------------------------------------
# Requires submodule directory to exist under submodules/ relative to this file, then sets VAR_NAME to its path.

function(dthelpers_resolve var_name submodule_name help_text)
    get_filename_component(_self_name "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" NAME)
    if("${submodule_name}" STREQUAL "${_self_name}")
        set(_submodule_path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    else()
        set(_submodule_path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/submodules/${submodule_name}")
    endif()
    if(NOT EXISTS "${_submodule_path}")
        message(FATAL_ERROR
            "${var_name}: submodule '${submodule_name}' not found at '${_submodule_path}' (${help_text}).")
    endif()

    set(${var_name} "${_submodule_path}" PARENT_SCOPE)
endfunction()
