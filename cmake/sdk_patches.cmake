# Local changes to the ReXGlue SDK live in patches/rexglue-sdk/*.patch
# (regenerated with tools/export_sdk_patch.sh) so the submodule can keep
# tracking upstream. Each patch is applied to a clean SDK checkout. A checkout
# that still has an older committed version of a patch applied (an existing
# clone after a pull) gets that version reversed and the current one applied.
# A tree with any other local changes is left alone (SDK development in
# progress).
#
# Included from CMakeLists.txt; also runs on its own:
#   cmake -DTRIALSHD_ROOT=<repo> -P cmake/sdk_patches.cmake

if(NOT TRIALSHD_ROOT)
    set(TRIALSHD_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(TRIALSHD_ROOT "${TRIALSHD_ROOT}" ABSOLUTE)
set(_sdk_dir "${TRIALSHD_ROOT}/thirdparty/rexglue-sdk")
find_package(Git REQUIRED)

# Runs git in the SDK tree; sets <result> to its exit code.
function(_trialshd_sdk_git result)
    execute_process(COMMAND ${GIT_EXECUTABLE} ${ARGN}
                    WORKING_DIRECTORY ${_sdk_dir}
                    RESULT_VARIABLE _r OUTPUT_QUIET ERROR_QUIET)
    set(${result} ${_r} PARENT_SCOPE)
endfunction()

# Sets <out> to the SDK tree's changes to tracked files ("" when clean).
function(_trialshd_sdk_changes out)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                    WORKING_DIRECTORY ${_sdk_dir}
                    OUTPUT_VARIABLE _changes OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out} "${_changes}" PARENT_SCOPE)
endfunction()

# Drops index entries for files missing from the SDK tree. tools/export_sdk_patch.sh
# marks the files a patch adds with `git add -N`; once a reversed patch deletes
# them those entries would otherwise read as local changes.
function(_trialshd_sdk_forget_deleted)
    execute_process(COMMAND ${GIT_EXECUTABLE} ls-files --deleted
                    WORKING_DIRECTORY ${_sdk_dir}
                    OUTPUT_VARIABLE _deleted OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _deleted STREQUAL "")
        string(REPLACE "\n" ";" _deleted "${_deleted}")
        _trialshd_sdk_git(_r reset -q -- ${_deleted})
    endif()
endfunction()

file(GLOB _patches "${TRIALSHD_ROOT}/patches/rexglue-sdk/*.patch")
list(SORT _patches)
foreach(_patch IN LISTS _patches)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        # A pulled patch update re-runs the configure step.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_patch})
    endif()
    _trialshd_sdk_git(_already_applied apply --reverse --check ${_patch})
    if(_already_applied EQUAL 0)
        continue()
    endif()

    set(_previous "")
    _trialshd_sdk_changes(_sdk_changes)
    if(NOT _sdk_changes STREQUAL "")
        # Look for the committed version of this patch that the tree has applied.
        file(RELATIVE_PATH _rel ${TRIALSHD_ROOT} ${_patch})
        execute_process(COMMAND ${GIT_EXECUTABLE} log --format=%H -- ${_rel}
                        WORKING_DIRECTORY ${TRIALSHD_ROOT}
                        OUTPUT_VARIABLE _revs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REPLACE "\n" ";" _revs "${_revs}")
        get_filename_component(_name ${_patch} NAME)
        set(_old "${CMAKE_CURRENT_BINARY_DIR}/previous-${_name}")
        foreach(_rev IN LISTS _revs)
            execute_process(COMMAND ${GIT_EXECUTABLE} show ${_rev}:${_rel}
                            WORKING_DIRECTORY ${TRIALSHD_ROOT}
                            OUTPUT_FILE ${_old} RESULT_VARIABLE _r ERROR_QUIET)
            if(NOT _r EQUAL 0)
                continue()
            endif()
            _trialshd_sdk_git(_r apply --reverse --check ${_old})
            if(_r EQUAL 0)
                _trialshd_sdk_git(_r apply --reverse ${_old})
                _trialshd_sdk_forget_deleted()
                _trialshd_sdk_changes(_sdk_changes)
                if(_r EQUAL 0 AND _sdk_changes STREQUAL "")
                    set(_previous ${_old})
                    string(SUBSTRING ${_rev} 0 7 _short)
                    message(STATUS "Reversed SDK patch ${_name} as of ${_short}")
                elseif(_r EQUAL 0)
                    # More than that version's changes: keep the tree as it was.
                    _trialshd_sdk_git(_r apply ${_old})
                endif()
                break()
            endif()
        endforeach()
        if(NOT _previous)
            file(REMOVE ${_old})
            message(STATUS "SDK tree has local changes; not re-applying ${_patch}")
            continue()
        endif()
    endif()

    _trialshd_sdk_git(_apply_result apply ${_patch})
    if(NOT _apply_result EQUAL 0)
        if(_previous)
            _trialshd_sdk_git(_r apply ${_previous})
        endif()
        message(FATAL_ERROR "Failed to apply SDK patch ${_patch}")
    endif()
    if(_previous)
        file(REMOVE ${_previous})
    endif()
    message(STATUS "Applied SDK patch ${_patch}")
endforeach()
