# Warren -- a game engine with first-class portals.
#
# Two graphics backends, both first class. Two shader variants, both
# generated from one source. Windows and Linux.
cmake_minimum_required(VERSION 3.20)
project(warren VERSION 0.1.0 LANGUAGES CXX C)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(WARREN_OPENGL   "Build the OpenGL 4.5 backend"   ON)
option(WARREN_VULKAN   "Build the Vulkan 1.3 backend"   ON)
option(WARREN_PYTHON   "Embed CPython for scripting"    ON)
option(WARREN_TESTS    "Build the tests"                ON)
option(WARREN_PLUGINS  "Build the bundled plugins"      ON)
option(WARREN_OPTIMISE_SHADERS "Run spirv-opt on shaders" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ---------------------------------------------------------------- codegen
#
# Three generators, all driven by manifests checked into the tree, all
# producing sources that are build products and not committed. A name
# that does not exist in the system header is a build failure here
# rather than a null function pointer at run time.

set(GEN_STAMP_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_STAMP_DIR})

set(GL_GEN ${CMAKE_SOURCE_DIR}/src/rhi/gl/glfn.h ${CMAKE_SOURCE_DIR}/src/rhi/gl/glfn.cpp)
add_custom_command(
  OUTPUT ${GL_GEN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_gl.py
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_gl.py ${CMAKE_SOURCE_DIR}/tools/glfns.txt
          ${CMAKE_SOURCE_DIR}/third_party/GL/glcorearb.h
  COMMENT "OpenGL loader from tools/glfns.txt" VERBATIM)

set(VK_GEN ${CMAKE_SOURCE_DIR}/src/rhi/vk/vkfn.h ${CMAKE_SOURCE_DIR}/src/rhi/vk/vkfn.cpp)
add_custom_command(
  OUTPUT ${VK_GEN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_vk.py
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_vk.py ${CMAKE_SOURCE_DIR}/tools/vkfns.txt
  COMMENT "Vulkan loader from tools/vkfns.txt" VERBATIM)

file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/render/shaders/*.glsl)
set(SHADER_GEN ${CMAKE_SOURCE_DIR}/src/render/shaders/generated/shaders.h
               ${CMAKE_SOURCE_DIR}/src/render/shaders/generated/shaders.cpp)
set(SHADER_ARGS "")
if(WARREN_OPTIMISE_SHADERS)
  set(SHADER_ARGS --optimise)
endif()
add_custom_command(
  OUTPUT ${SHADER_GEN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_shaders.py ${SHADER_ARGS}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_shaders.py ${SHADER_SOURCES}
  COMMENT "Shaders: GLSL -> SPIR-V (Vulkan) -> GLSL 460 (OpenGL)" VERBATIM)

add_custom_target(warren_codegen DEPENDS ${GL_GEN} ${VK_GEN} ${SHADER_GEN})

# ----------------------------------------------------------------- sources

set(WARREN_DIRS core core/math platform audio net ui editor render
                  render/shaders/generated scene physics script plugin
                  resource agent app)
set(WARREN_SRC "")
foreach(d ${WARREN_DIRS})
  file(GLOB s CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/${d}/*.cpp)
  list(APPEND WARREN_SRC ${s})
endforeach()
file(GLOB s CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/rhi/*.cpp)
list(APPEND WARREN_SRC ${s})
if(WARREN_OPENGL)
  file(GLOB s CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/rhi/gl/*.cpp)
  list(APPEND WARREN_SRC ${s})
endif()
if(WARREN_VULKAN)
  file(GLOB s CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/rhi/vk/*.cpp)
  list(APPEND WARREN_SRC ${s})
endif()

# SHARED WHEN PLUGINS ARE ON, and that is not a packaging preference.
#
# A plugin registers classes into ClassDB, logs through the engine's
# logger and allocates from its pools. Link the engine statically into
# both the runtime and the plugin and there are TWO of each -- the
# plugin's classes register into a registry nobody reads, and its log
# lines go somewhere nobody prints. One shared library means one of
# everything.
if(WARREN_PLUGINS)
  add_library(warren SHARED ${WARREN_SRC})
  set_target_properties(warren PROPERTIES
    ENABLE_EXPORTS ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
else()
  add_library(warren STATIC ${WARREN_SRC})
endif()
add_dependencies(warren warren_codegen)
target_include_directories(warren PUBLIC
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

target_compile_definitions(warren PUBLIC
  WARREN_OPENGL=$<BOOL:${WARREN_OPENGL}>
  WARREN_VULKAN=$<BOOL:${WARREN_VULKAN}>
  WARREN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  WARREN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  WARREN_VERSION_PATCH=${PROJECT_VERSION_PATCH})

if(MSVC)
  target_compile_options(warren PRIVATE /W4 /permissive- /Zc:preprocessor
                                          /wd4100 /wd4201 /utf-8 /bigobj)
  target_compile_definitions(warren PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
                                             _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(warren PRIVATE -Wall -Wextra -Wno-unused-parameter
                                          -Wno-missing-field-initializers)
endif()

# ------------------------------------------------------------------- SDL2
find_package(SDL2 QUIET)
if(SDL2_FOUND AND TARGET SDL2::SDL2)
  target_link_libraries(warren PUBLIC SDL2::SDL2)
elseif(SDL2_FOUND)
  target_include_directories(warren PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(warren PUBLIC ${SDL2_LIBRARIES})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  target_link_libraries(warren PUBLIC PkgConfig::SDL2)
endif()

# ------------------------------------------------------------- Vulkan
# HEADERS ONLY. The loader is opened with dlopen at run time, so a
# machine with no Vulkan driver runs the OpenGL backend instead of
# failing to start -- which is the whole reason for tools/gen_vk.py.
if(WARREN_VULKAN)
  find_package(Vulkan QUIET)
  if(Vulkan_INCLUDE_DIRS)
    target_include_directories(warren PUBLIC ${Vulkan_INCLUDE_DIRS})
  elseif(NOT EXISTS /usr/include/vulkan/vulkan.h)
    message(WARNING "Vulkan headers not found; building without the Vulkan backend")
    set(WARREN_VULKAN OFF)
  endif()
endif()

if(WIN32)
  target_link_libraries(warren PUBLIC opengl32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(warren PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ----------------------------------------------------------------- CPython
if(WARREN_PYTHON)
  find_package(Python3 COMPONENTS Development REQUIRED)
  target_link_libraries(warren PUBLIC Python3::Python)
  target_compile_definitions(warren PUBLIC WARREN_PYTHON=1)
endif()

# ----------------------------------------------------------------- runtime
if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
  add_executable(warren_run ${CMAKE_SOURCE_DIR}/src/main.cpp)
  target_link_libraries(warren_run PRIVATE warren)
  set_target_properties(warren_run PROPERTIES OUTPUT_NAME warren)
endif()

# ----------------------------------------------------------------- plugins
#
# Each lands in bin/plugins, which is where the runtime looks.
set(WARREN_PLUGIN_DIR ${CMAKE_BINARY_DIR}/bin/plugins)
function(warren_add_plugin name)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE warren)
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${WARREN_PLUGIN_DIR})
  if(NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter
                                           -Wno-missing-field-initializers)
  endif()
endfunction()

if(WARREN_PLUGINS AND EXISTS ${CMAKE_SOURCE_DIR}/plugins)
  file(GLOB plugin_dirs ${CMAKE_SOURCE_DIR}/plugins/*)
  foreach(p ${plugin_dirs})
    if(IS_DIRECTORY ${p} AND EXISTS ${p}/CMakeLists.txt)
      add_subdirectory(${p})
    endif()
  endforeach()
endif()

# ------------------------------------------------------------------- tests
if(WARREN_TESTS)
  enable_testing()
  file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
  foreach(t ${test_sources})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t})
    target_link_libraries(${name} PRIVATE warren)
    add_test(NAME ${name} COMMAND ${name})
    # 77 is the POSIX convention for "skipped", which a test uses when
    # the thing it covers was not built (Python, say). A skip is not a
    # pass and it is not a failure; ctest should say so.
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
endif()

message(STATUS "")
message(STATUS "Warren ${PROJECT_VERSION}")
message(STATUS "  OpenGL backend : ${WARREN_OPENGL}")
message(STATUS "  Vulkan backend : ${WARREN_VULKAN}")
message(STATUS "  Python scripts : ${WARREN_PYTHON}")
message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "")
