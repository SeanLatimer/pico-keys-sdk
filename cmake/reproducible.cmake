#
# Settings that make the firmware image depend only on the commit it is
# built from, not on when or where it is built.
#
# - Build date. pico-sdk stores __DATE__ in the binary info block. It is
#   replaced by a fixed date: SOURCE_DATE_EPOCH from the environment if set,
#   else -DPICOKEYS_BUILD_DATE=..., else the committer date of HEAD.
# - Paths. __FILE__ ends up in the image through log and assert strings.
#   The source, SDK and build directories are mapped to fixed prefixes.
#
# The build number (cmake/version.cmake) is the commit count, which is
# already deterministic for a full clone; see SET_BUILD_NUMBER.
#

option(PICOKEYS_REPRODUCIBLE "Fix the build date and strip local paths from the image" ON)

if(PICOKEYS_REPRODUCIBLE)
    # Date in __DATE__ format ("Mmm dd yyyy", day padded with a space).
    if(NOT PICOKEYS_BUILD_DATE)
        set(_epoch "$ENV{SOURCE_DATE_EPOCH}")
        if(NOT _epoch)
            execute_process(
                COMMAND git -c "safe.directory=${CMAKE_SOURCE_DIR}" log -1 --format=%ct
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE _epoch
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        endif()
        if(_epoch MATCHES "^[0-9]+$")
            # string(TIMESTAMP) honours SOURCE_DATE_EPOCH, so set it for the call.
            set(_saved_epoch "$ENV{SOURCE_DATE_EPOCH}")
            set(ENV{SOURCE_DATE_EPOCH} "${_epoch}")
            string(TIMESTAMP _month "%m" UTC)
            string(TIMESTAMP _day "%d" UTC)
            string(TIMESTAMP _year "%Y" UTC)
            set(ENV{SOURCE_DATE_EPOCH} "${_saved_epoch}")
            set(_months Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec)
            math(EXPR _mi "${_month} - 1")
            list(GET _months ${_mi} _mname)
            string(REGEX REPLACE "^0" " " _day "${_day}")
            set(PICOKEYS_BUILD_DATE "${_mname} ${_day} ${_year}")
        else()
            set(PICOKEYS_BUILD_DATE "Jan  1 1970")
            message(WARNING "No SOURCE_DATE_EPOCH and no git history: build date set to ${PICOKEYS_BUILD_DATE}")
        endif()
    endif()
    message(STATUS "Build date:\t\t ${PICOKEYS_BUILD_DATE}")

    # Later matches win in GCC, so the outer source directory comes first and
    # the directories inside it (SDK, build) after it.
    get_filename_component(_sdk_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(_prefix_maps
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=./"
        "-ffile-prefix-map=${_sdk_dir}/=pico-keys-sdk/"
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=build/"
    )
    if(PICO_SDK_PATH)
        list(APPEND _prefix_maps "-ffile-prefix-map=${PICO_SDK_PATH}/=pico-sdk/")
    endif()
    if(NOT MSVC)
        add_compile_options(${_prefix_maps})
    endif()
    add_compile_definitions("PICO_PROGRAM_BUILD_DATE=\"${PICOKEYS_BUILD_DATE}\"")

    # The firmware target exists before this file is included, and directory
    # compile options (unlike definitions) only apply to targets created
    # afterwards.
    if(TARGET ${CMAKE_PROJECT_NAME})
        if(NOT MSVC)
            target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE ${_prefix_maps})
        endif()
    endif()
endif()
