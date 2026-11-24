cmake_minimum_required(VERSION 4.3)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP "${VER_RAW}" HYPRTOOLKIT_LUA_VERSION)

project(
  hyprtoolkit-lua
  VERSION ${HYPRTOOLKIT_LUA_VERSION}
  DESCRIPTION "Lua 5.5 bindings for Hyprtoolkit"
  LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(HYPRTOOLKIT_PREFIX "" CACHE PATH
    "Prefix containing an external Hyprtoolkit installation")
if(HYPRTOOLKIT_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${HYPRTOOLKIT_PREFIX}")
endif()

find_package(PkgConfig REQUIRED)
find_package(Lua 5.5 EXACT REQUIRED)
pkg_check_modules(HYPRTOOLKIT REQUIRED IMPORTED_TARGET hyprtoolkit>=0.6.0)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

set(HYPRTOOLKIT_LUA_SOURCES
    src/LuaBindings.cpp
    src/LuaState.cpp
    src/bindings/Core.cpp
    src/bindings/ElementBuilders.cpp
    src/bindings/Elements.cpp
    src/bindings/Types.cpp
    src/bindings/Window.cpp)

add_library(hyprtoolkit-lua SHARED ${HYPRTOOLKIT_LUA_SOURCES})
target_compile_features(hyprtoolkit-lua PUBLIC cxx_std_23)
target_include_directories(
  hyprtoolkit-lua
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" ${LUA_INCLUDE_DIR})
target_compile_definitions(
  hyprtoolkit-lua
  PRIVATE HYPRTOOLKIT_LUA_VERSION="${HYPRTOOLKIT_LUA_VERSION}"
          HYPRTOOLKIT_LUA_UPSTREAM_BRANCH="main")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    hyprtoolkit-lua
    PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-value
            -Wno-missing-field-initializers -Wpedantic -Werror)
endif()
target_link_libraries(
  hyprtoolkit-lua
  PUBLIC PkgConfig::HYPRTOOLKIT ${LUA_LIBRARIES}
  PRIVATE PkgConfig::LIBDRM)
set_target_properties(
  hyprtoolkit-lua PROPERTIES VERSION ${HYPRTOOLKIT_LUA_VERSION} SOVERSION 0)

add_library(hyprtoolkit-lua-module MODULE src/LuaModule.cpp)
target_link_libraries(hyprtoolkit-lua-module PRIVATE hyprtoolkit-lua)
set_target_properties(
  hyprtoolkit-lua-module
  PROPERTIES OUTPUT_NAME hyprtoolkit PREFIX "" CXX_VISIBILITY_PRESET hidden)

add_executable(hyprtoolkit-lua-runner runner/main.cpp)
target_link_libraries(hyprtoolkit-lua-runner PRIVATE hyprtoolkit-lua)
set_target_properties(hyprtoolkit-lua-runner PROPERTIES OUTPUT_NAME hyprtoolkit-lua)

set(HYPRTOOLKIT_LUA_INSTALL_CMOD_DIR "${CMAKE_INSTALL_LIBDIR}/lua/5.5" CACHE PATH
    "Installation directory for the Lua C module")
if(IS_ABSOLUTE "${HYPRTOOLKIT_LUA_INSTALL_CMOD_DIR}")
  set(HYPRTOOLKIT_LUA_INSTALL_CMOD_FULL_DIR
      "${HYPRTOOLKIT_LUA_INSTALL_CMOD_DIR}")
else()
  set(HYPRTOOLKIT_LUA_INSTALL_CMOD_FULL_DIR
      "${CMAKE_INSTALL_PREFIX}/${HYPRTOOLKIT_LUA_INSTALL_CMOD_DIR}")
endif()
file(RELATIVE_PATH HYPRTOOLKIT_LUA_RUNNER_LIBDIR "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_LIBDIR}")
file(RELATIVE_PATH HYPRTOOLKIT_LUA_MODULE_LIBDIR
     "${HYPRTOOLKIT_LUA_INSTALL_CMOD_FULL_DIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
set_target_properties(
  hyprtoolkit-lua-runner
  PROPERTIES INSTALL_RPATH "$ORIGIN/${HYPRTOOLKIT_LUA_RUNNER_LIBDIR}")
set_target_properties(
  hyprtoolkit-lua-module
  PROPERTIES INSTALL_RPATH "$ORIGIN/${HYPRTOOLKIT_LUA_MODULE_LIBDIR}")

configure_file(hyprtoolkit-lua.pc.in hyprtoolkit-lua.pc @ONLY)

install(
  TARGETS hyprtoolkit-lua hyprtoolkit-lua-runner
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS hyprtoolkit-lua-module
        LIBRARY DESTINATION ${HYPRTOOLKIT_LUA_INSTALL_CMOD_DIR})
install(DIRECTORY include/hyprtoolkit-lua DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hyprtoolkit-lua.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

option(HYPRTOOLKIT_LUA_ENABLE_GUI_TESTS
       "Enable graphical tests that require an active Wayland session" OFF)
option(HYPRTOOLKIT_LUA_ENABLE_EMBEDDED_TESTS
       "Enable headless EGL embedding tests" OFF)

if(BUILD_TESTING)
  add_executable(bindings-test tests/Bindings.cpp)
  target_link_libraries(bindings-test PRIVATE hyprtoolkit-lua PkgConfig::LIBDRM)
  target_include_directories(bindings-test PRIVATE src ${LUA_INCLUDE_DIR})
  add_test(NAME bindings COMMAND bindings-test)
  set_tests_properties(bindings PROPERTIES TIMEOUT 10)

  add_test(NAME api-surface
           COMMAND hyprtoolkit-lua-runner
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/api_surface.lua")

  find_program(LUA_EXECUTABLE NAMES lua5.5 lua55 lua)
  if(NOT LUA_EXECUTABLE)
    message(FATAL_ERROR "A Lua 5.5 interpreter is required for module tests")
  endif()
  execute_process(
    COMMAND "${LUA_EXECUTABLE}" -e "assert(_VERSION == 'Lua 5.5')"
    RESULT_VARIABLE LUA_INTERPRETER_STATUS
    ERROR_VARIABLE LUA_INTERPRETER_ERROR)
  if(NOT LUA_INTERPRETER_STATUS STREQUAL "0")
    message(FATAL_ERROR "Module tests require Lua 5.5: ${LUA_INTERPRETER_ERROR}")
  endif()

  add_test(NAME module-loading
           COMMAND "${LUA_EXECUTABLE}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_loading.lua")
  set_tests_properties(
    module-loading
    PROPERTIES ENVIRONMENT
                "LUA_CPATH=$<TARGET_FILE_DIR:hyprtoolkit-lua-module>/?.so;;")

  if(HYPRTOOLKIT_LUA_ENABLE_EMBEDDED_TESTS)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)
    add_executable(embedded-test tests/Embedded.cpp)
    target_include_directories(embedded-test PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(embedded-test PRIVATE hyprtoolkit-lua
                          PkgConfig::LIBDRM PkgConfig::EGL PkgConfig::GLES)
    add_test(NAME embedded COMMAND embedded-test)
    set_tests_properties(embedded PROPERTIES LABELS headless
                         SKIP_RETURN_CODE 77 TIMEOUT 30)
  endif()

  if(HYPRTOOLKIT_LUA_ENABLE_GUI_TESTS)
    add_test(NAME gui-smoke
             COMMAND hyprtoolkit-lua-runner
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_smoke.lua")
    set_tests_properties(gui-smoke PROPERTIES LABELS gui)
  endif()
endif()
