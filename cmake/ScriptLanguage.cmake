# The hook a language plugs into on the build side.
#
#   script_language(NAME basic OPTION SCRIPT_LANG_BASIC ENGINE TYPED
#                    SOURCES src/lang/basic/lexer.cpp ...)
#
# records a front end: when its option is on, its sources join the library and
# the engine it names is built. After every call, `script_finish_languages()`
# derives SCRIPT_ENGINE_TYPED / SCRIPT_ENGINE_DYNAMIC and the source list.
# The C++ side of the same hook is `script::internal::LanguageInfo`; see
# docs/adding-a-language.md for the whole recipe.

set(SCRIPT_LANGUAGE_SOURCES "")
set(SCRIPT_LANGUAGE_NAMES "")
set(SCRIPT_ENGINE_TYPED OFF)
set(SCRIPT_ENGINE_DYNAMIC OFF)

function(script_language)
    cmake_parse_arguments(L "" "NAME;OPTION;ENGINE" "SOURCES" ${ARGN})
    if(NOT L_NAME OR NOT L_OPTION OR NOT L_ENGINE)
        message(FATAL_ERROR "script_language needs NAME, OPTION and ENGINE")
    endif()
    if(NOT ${L_OPTION})
        return()
    endif()
    list(APPEND SCRIPT_LANGUAGE_SOURCES ${L_SOURCES})
    list(APPEND SCRIPT_LANGUAGE_NAMES ${L_NAME})
    set(SCRIPT_LANGUAGE_SOURCES "${SCRIPT_LANGUAGE_SOURCES}" PARENT_SCOPE)
    set(SCRIPT_LANGUAGE_NAMES "${SCRIPT_LANGUAGE_NAMES}" PARENT_SCOPE)
    if(L_ENGINE STREQUAL "TYPED")
        set(SCRIPT_ENGINE_TYPED ON PARENT_SCOPE)
    elseif(L_ENGINE STREQUAL "DYNAMIC")
        set(SCRIPT_ENGINE_DYNAMIC ON PARENT_SCOPE)
    else()
        message(FATAL_ERROR "script_language: ENGINE must be TYPED or DYNAMIC, not ${L_ENGINE}")
    endif()
endfunction()

function(script_finish_languages)
    string(REPLACE ";" ", " names "${SCRIPT_LANGUAGE_NAMES}")
    set(SCRIPT_LANGUAGE_NAMES "${names}" PARENT_SCOPE)
    # Languages that share a front end (C and C++) list the same sources.
    list(REMOVE_DUPLICATES SCRIPT_LANGUAGE_SOURCES)
    set(SCRIPT_LANGUAGE_SOURCES "${SCRIPT_LANGUAGE_SOURCES}" PARENT_SCOPE)
endfunction()
