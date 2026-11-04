cmake_minimum_required(VERSION 3.16)
project(libscript VERSION 0.1.0 LANGUAGES CXX)

# ---- what to build -----------------------------------------------------------
option(SCRIPT_BUILD_TESTS "Build the test suites" ON)
option(SCRIPT_BUILD_EXAMPLES "Build the examples" ON)
option(SCRIPT_BUILD_TOOLS "Build the script command line tool" ON)
option(SCRIPT_BUILD_BENCHMARKS "Build bench/, what a long run costs" OFF)
option(SCRIPT_BUILD_SHARED "Build a shared library instead of a static one" OFF)
option(SCRIPT_ENABLE_ASAN "Build with address+undefined sanitizers" OFF)
option(SCRIPT_WARNINGS_AS_ERRORS "Turn compiler warnings into errors" OFF)

# ---- which languages ---------------------------------------------------------
# Every language is on by default. Turning one off drops its front end, its
# tests and its examples; an engine is dropped once no language needs it.
option(SCRIPT_LANG_CSHARP "C#: statically typed, on the typed engine" ON)
option(SCRIPT_LANG_TYPESCRIPT "TypeScript: checked, erased, on the dynamic engine" ON)
option(SCRIPT_LANG_JAVASCRIPT "JavaScript: on the dynamic engine" ON)
option(SCRIPT_LANG_PYTHON "Python: on the dynamic engine" ON)
option(SCRIPT_LANG_C "C: statically typed, on the typed engine" ON)
option(SCRIPT_LANG_CPP "C++: the C front end's dialect with classes and the standard containers, on the typed engine" ON)
option(SCRIPT_ENABLE_JIT "Compile the arm64 and x86-64 code generators for the typed engine" ON)

include(cmake/ScriptLanguage.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- libcore ------------------------------------------------------------------
# The allocators, the Unicode and number conversions, the page protection and the build flags are libcore's: a sibling
# checkout by default, or an installed package with -DSCRIPT_LIBCORE_DIR="".
if(SCRIPT_ENABLE_ASAN AND NOT MSVC)
    set(CORE_SANITIZE "address,undefined" CACHE STRING "" FORCE)
endif()
set(SCRIPT_LIBCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libcore" CACHE PATH "Path to a libcore checkout; empty: find_package(libcore)")
if(SCRIPT_LIBCORE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${SCRIPT_LIBCORE_DIR}/buildkit/cmake")
    include(CoreSanitize)   # before libcore, so that it builds with the same sanitizers
    if(NOT TARGET core::mem)
        set(CORE_BUILD_TESTS OFF CACHE BOOL "")
        add_subdirectory("${SCRIPT_LIBCORE_DIR}" libcore)
    endif()
else()
    find_package(libcore CONFIG REQUIRED)
    include(CoreSanitize)
endif()
include(CoreCompiler)

# One set of warning flags, applied to every target this project builds -- library, tests, examples and tool alike.
function(script_set_warnings target)
    # GCC counts a scoped enum's members as shadowing a namespace-scope name of the same spelling, which `TypeKind::Char`
    # next to `using Char` trips on constantly. The local form catches the shadowing that actually bites.
    core_target_defaults(${target} SHADOW_LOCAL)
    if(MSVC)
        target_compile_options(${target} PRIVATE /Zc:__cplusplus /bigobj)
    endif()
    if(SCRIPT_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE $<IF:$<BOOL:${MSVC}>,/WX,-Werror>)
    endif()
endfunction()

# Whole functions and data can be dropped at link time, for anyone linking the archive with --gc-sections.
if(NOT MSVC)
    add_compile_options(-ffunction-sections -fdata-sections)
endif()

# ---- the engines and the languages ------------------------------------------
# A language declares which engine it runs on; the engine's sources are pulled
# in once any of its languages is enabled. `script_language` records the
# choice, and the table in src/core/language.cpp follows the same options.
set(SCRIPT_CORE_SOURCES
    src/core/strings.cpp
    src/core/payload.cpp
    src/core/heap.cpp
    src/core/language.cpp
    src/core/api.cpp
)

# The C# front end's lexer, parser, checker, cloner and record expansion
# resolve the typed AST for every language that produces it (C#, C and C++): the
# checker synthesizes record members by parsing C# text, so all of it belongs
# to the engine. The C# *language* option adds only the registration.
set(SCRIPT_TYPED_SOURCES
    src/lang/csharp/lexer.cpp
    src/lang/csharp/parser.cpp
    src/lang/csharp/checker.cpp
    src/lang/csharp/clone.cpp
    src/lang/csharp/records.cpp
    src/typed/runtime.cpp
    src/typed/numconv.cpp
    src/typed/types.cpp
    src/typed/emitter.cpp
    src/typed/object.cpp
    src/typed/vm.cpp
    src/typed/module.cpp
    src/typed/jit/jit.cpp
    src/typed/jit/jit_arm64.cpp
    src/typed/jit/jit_x64.cpp
    src/typed/builtins/core.cpp
    src/typed/builtins/string.cpp
    src/typed/builtins/math.cpp
    src/typed/builtins/collections.cpp
    src/typed/builtins/tuples.cpp
)

set(SCRIPT_DYNAMIC_SOURCES
    src/dyn/runtime.cpp
    src/dyn/numconv.cpp
    src/dyn/object.cpp
    src/dyn/vm.cpp
    src/dyn/compiler.cpp
    src/dyn/builtins/core.cpp
    src/dyn/builtins/array.cpp
    src/dyn/builtins/string.cpp
    src/dyn/builtins/math_json.cpp
    src/dyn/builtins/collections.cpp
    src/lang/ecmascript/lexer.cpp
    src/lang/ecmascript/parser.cpp
)

script_language(NAME csharp OPTION SCRIPT_LANG_CSHARP ENGINE TYPED
    SOURCES
        src/lang/csharp/csharp.cpp)
script_language(NAME typescript OPTION SCRIPT_LANG_TYPESCRIPT ENGINE DYNAMIC
    SOURCES
        src/lang/typescript/types.cpp
        src/lang/typescript/lib.cpp
        src/lang/typescript/checker.cpp
        src/lang/typescript/erase.cpp
        src/lang/typescript/typescript.cpp)
script_language(NAME javascript OPTION SCRIPT_LANG_JAVASCRIPT ENGINE DYNAMIC
    SOURCES
        src/lang/javascript/javascript.cpp)
script_language(NAME python OPTION SCRIPT_LANG_PYTHON ENGINE DYNAMIC
    SOURCES
        src/lang/python/lexer.cpp
        src/lang/python/parser.cpp
        src/lang/python/python.cpp)
# C and C++ share one front end: the lexer and parser carry a dialect flag,
# the way the ECMAScript parser does for TypeScript and JavaScript. Each
# language adds its registration; the shared sources are listed once.
set(SCRIPT_C_FAMILY_SOURCES
    src/lang/c/lexer.cpp
    src/lang/c/parser.cpp
    src/lang/c/cxx.cpp
    src/lang/c/libc.cpp
    src/lang/c/frontend.cpp
)
script_language(NAME c OPTION SCRIPT_LANG_C ENGINE TYPED
    SOURCES
        ${SCRIPT_C_FAMILY_SOURCES}
        src/lang/c/c.cpp)
script_language(NAME cpp OPTION SCRIPT_LANG_CPP ENGINE TYPED
    SOURCES
        ${SCRIPT_C_FAMILY_SOURCES}
        src/lang/cpp/cpp.cpp)

script_finish_languages()  # sets SCRIPT_ENGINE_TYPED / SCRIPT_ENGINE_DYNAMIC and SCRIPT_LANGUAGE_SOURCES

if(NOT SCRIPT_LANGUAGE_SOURCES)
    message(FATAL_ERROR "libscript: every language is disabled; enable at least one SCRIPT_LANG_* option")
endif()
if(SCRIPT_ENABLE_JIT AND NOT SCRIPT_ENGINE_TYPED)
    set(SCRIPT_ENABLE_JIT OFF)
endif()

set(SCRIPT_SOURCES ${SCRIPT_CORE_SOURCES} ${SCRIPT_LANGUAGE_SOURCES})
if(SCRIPT_ENGINE_TYPED)
    list(APPEND SCRIPT_SOURCES ${SCRIPT_TYPED_SOURCES})
endif()
if(SCRIPT_ENGINE_DYNAMIC)
    list(APPEND SCRIPT_SOURCES ${SCRIPT_DYNAMIC_SOURCES})
endif()

# ---- the generated configuration header ---------------------------------------
foreach(flag SCRIPT_LANG_CSHARP SCRIPT_LANG_TYPESCRIPT SCRIPT_LANG_JAVASCRIPT
             SCRIPT_LANG_PYTHON SCRIPT_LANG_C SCRIPT_LANG_CPP SCRIPT_ENGINE_TYPED
             SCRIPT_ENGINE_DYNAMIC SCRIPT_ENABLE_JIT)
    if(${flag})
        set(${flag}_01 1)
    else()
        set(${flag}_01 0)
    endif()
endforeach()
configure_file(include/script/config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/script/config.hpp @ONLY)

# ---- the library -------------------------------------------------------------
if(SCRIPT_BUILD_SHARED)
    set(SCRIPT_LIBRARY_KIND SHARED)
else()
    set(SCRIPT_LIBRARY_KIND STATIC)
endif()

add_library(script ${SCRIPT_LIBRARY_KIND} ${SCRIPT_SOURCES})
add_library(script::script ALIAS script)

target_include_directories(script
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:include>
)
target_compile_features(script PUBLIC cxx_std_17)

# The host architecture the code generators target. Kept out of the sources so
# a cross build reports the target, not the builder.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    target_compile_definitions(script PRIVATE SCRIPT_ARCH_ARM64=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_definitions(script PRIVATE SCRIPT_ARCH_X64=1)
endif()

if(MSVC)
    target_compile_definitions(script PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
script_set_warnings(script)

find_package(Threads REQUIRED)
# Public because a static archive needs them at the final link; the public headers do not include them. An installed
# package gets them from cmake/scriptConfig.cmake.in, as libcore's own targets rather than this build's aliases.
target_link_libraries(script PUBLIC Threads::Threads
    $<BUILD_INTERFACE:core::mem> $<BUILD_INTERFACE:core::sys> $<BUILD_INTERFACE:core::base>)
# Header-only use: JSON escaping.
target_include_directories(script PRIVATE $<TARGET_PROPERTY:core::json,INTERFACE_INCLUDE_DIRECTORIES>)

set_target_properties(script PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME script
    POSITION_INDEPENDENT_CODE ON
)

message(STATUS "libscript: languages ${SCRIPT_LANGUAGE_NAMES}; typed engine ${SCRIPT_ENGINE_TYPED}; dynamic engine ${SCRIPT_ENGINE_DYNAMIC}; jit ${SCRIPT_ENABLE_JIT}")

# ---- the rest ----------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/scriptConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/scriptConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/script)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/scriptConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)

if(NOT SCRIPT_LIBCORE_DIR AND (SCRIPT_BUILD_TESTS OR SCRIPT_BUILD_BENCHMARKS))
    message(FATAL_ERROR "the tests and benchmarks use libcore's test harness: point SCRIPT_LIBCORE_DIR at a checkout, or turn them off")
endif()
if(SCRIPT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(SCRIPT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(SCRIPT_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(SCRIPT_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

install(TARGETS script EXPORT script-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/script DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "internal" EXCLUDE
    PATTERN "config.hpp.in" EXCLUDE)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/script/config.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/script)
install(EXPORT script-targets NAMESPACE script:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/script)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/scriptConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/scriptConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/script)
