cmake_minimum_required(VERSION 3.5)
cmake_policy(SET CMP0022 NEW)

project(sampgdk)

# Whether this is the top-level project (standalone build) or sampgdk is
# pulled in from another project via add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SAMPGDK_TOP_LEVEL ON)
else()
  set(SAMPGDK_TOP_LEVEL OFF)
endif()

# cmake/modules is the samp-cmake-modules submodule (shared build modules);
# cmake holds sampgdk-specific files (packaging config, toolchain).
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

option(SAMPGDK_STATIC "Build static library instead" OFF)
option(SAMPGDK_BUILD_PLUGINS "Build example plugins" OFF)
option(SAMPGDK_BUILD_DOCS "Build Doxygen documentation" ON)
option(SAMPGDK_TINY "Build tiny version (callbacks only, no open.mp natives)" OFF)
option(SAMPGDK_ARCH "Target architecture (32 or 64)" 32)

set(SAMPGDK_VERSION_MAJOR 5)
set(SAMPGDK_VERSION_MINOR 0)
set(SAMPGDK_VERSION_PATCH 0)

set(SAMPGDK_VERSION ${SAMPGDK_VERSION_MAJOR})
set(SAMPGDK_VERSION ${SAMPGDK_VERSION}.${SAMPGDK_VERSION_MINOR})
set(SAMPGDK_VERSION ${SAMPGDK_VERSION}.${SAMPGDK_VERSION_PATCH})
set(SAMPGDK_VERSION ${SAMPGDK_VERSION}${SAMPGDK_VERSION_SUFFIX})

math(EXPR SAMPGDK_VERSION_ID "${SAMPGDK_VERSION_MAJOR} << 24 |
                              ${SAMPGDK_VERSION_MINOR} << 16 |
                              ${SAMPGDK_VERSION_PATCH} << 8")

if(NOT (MSVC_IDE AND MSVC_VERSION LESS 1600))
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

# SA-MP plugin SDK: an explicit SAMPSDK_DIR / SAMP_SDK_ROOT wins; otherwise
# fall back to the submodule checked out under deps/samp-plugin-sdk. This
# keeps both standalone builds and add_subdirectory consumers working.
if(NOT SAMPSDK_DIR AND NOT SAMP_SDK_ROOT AND NOT DEFINED ENV{SAMP_SDK_ROOT})
  set(SAMPSDK_DIR ${PROJECT_SOURCE_DIR}/deps/samp-plugin-sdk)
endif()
find_package(SAMPSDK REQUIRED)
find_package(PythonInterp 2.7 REQUIRED)

include_directories(
  ${SAMPSDK_INCLUDE_DIR}
  ${SAMPSDK_INCLUDE_DIR}/amx
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_BINARY_DIR}/include
  ${PROJECT_SOURCE_DIR}/lib/sampgdk
)
macro(sampgdk_add_subproject name)
  add_subdirectory(${name})
  file(RELATIVE_PATH folder ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
  set_property(TARGET ${name} PROPERTY FOLDER ${folder})
endmacro()

macro(sampgdk_get_property name var)
  get_property(${var} DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY ${name})
endmacro()

macro(sampgdk_set_property name value)
  set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY ${name} ${value})
endmacro()

macro(sampgdk_append_property name value)
  set_property(DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY ${name} ${value})
endmacro()

macro(sampgdk_add_sources)
  foreach(file ${ARGN})
    if(NOT IS_ABSOLUTE ${file})
      sampgdk_append_property(SAMPGDK_SOURCES
                              ${CMAKE_CURRENT_SOURCE_DIR}/${file})
    else()
      sampgdk_append_property(SAMPGDK_SOURCES ${file})
    endif()
  endforeach()
endmacro()

macro(sampgdk_get_sources var)
  sampgdk_get_property(SAMPGDK_SOURCES ${var})
endmacro()

macro(sampgdk_add_headers)
  foreach(file ${ARGN})
    if(NOT IS_ABSOLUTE ${file})
      sampgdk_append_property(SAMPGDK_HEADERS
                              ${CMAKE_CURRENT_SOURCE_DIR}/${file})
    else()
      sampgdk_append_property(SAMPGDK_HEADERS ${file})
    endif()
  endforeach()
endmacro()

macro(sampgdk_get_headers var)
  sampgdk_get_property(SAMPGDK_HEADERS ${var})
endmacro()

macro(sampgdk_add_libraries)
  foreach(library ${ARGN})
    sampgdk_append_property(SAMPGDK_LIBRARIES ${library})
  endforeach()
endmacro()

macro(sampgdk_get_libraries var)
  sampgdk_get_property(SAMPGDK_LIBRARIES ${var})
endmacro()

macro(sampgdk_add_api_gen module_name idl_file api_file)
  add_custom_command(
    OUTPUT ${api_file}
    COMMAND ${PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_BINARY_DIR}/scripts/python_wrapper.py
      ${PROJECT_SOURCE_DIR}/scripts/generate_code.py
      --module=${module_name}
      --idl=${idl_file}
      --api=${api_file}
      $<$<BOOL:${SAMPGDK_TINY}>:--tiny>
    DEPENDS ${idl_file} ${PROJECT_SOURCE_DIR}/scripts/generate_code.py)
  sampgdk_add_sources(${api_file})
endmacro()

macro(sampgdk_add_source_gen module_name idl_file source_file)
  add_custom_command(
    OUTPUT ${source_file}
    COMMAND ${PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_BINARY_DIR}/scripts/python_wrapper.py
      ${PROJECT_SOURCE_DIR}/scripts/generate_code.py
      --module=${module_name}
      --idl=${idl_file}
      --source=${source_file}
      $<$<BOOL:${SAMPGDK_TINY}>:--tiny>
    DEPENDS ${idl_file} ${PROJECT_SOURCE_DIR}/scripts/generate_code.py)
  sampgdk_add_sources(${source_file})
endmacro()

macro(sampgdk_add_header_gen module_name idl_file header_file)
  add_custom_command(
    OUTPUT ${header_file}
    COMMAND ${PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_BINARY_DIR}/scripts/python_wrapper.py
      ${PROJECT_SOURCE_DIR}/scripts/generate_code.py
      --module=${module_name}
      --idl=${idl_file}
      --header=${header_file}
    DEPENDS ${idl_file} ${PROJECT_SOURCE_DIR}/scripts/generate_code.py)
  sampgdk_add_headers(${header_file})
endmacro()

macro(sampgdk_add_init_gen target list_file source_file)
  add_custom_target(
    ${target}
    COMMAND ${PYTHON_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/scripts/generate_init.py
      --list=${list_file}
      --source=${source_file}
    DEPENDS ${list_file} ${PROJECT_SOURCE_DIR}/scripts/generate_init.py)
  sampgdk_add_sources(${source_file})
endmacro()

macro(sampgdk_add_exports_gen format output_file)
  add_custom_command(
    OUTPUT ${output_file}
    COMMAND ${PYTHON_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/scripts/generate_exports.py
      --format=${format}
      --output=${output_file}
      ${ARGN}
    DEPENDS ${ARGN} ${PROJECT_SOURCE_DIR}/scripts/generate_exports.py)
endmacro()

# cmake/ installs the CMake package-config files and exports the package,
# which only makes sense when building sampgdk standalone.
if(SAMPGDK_TOP_LEVEL)
  add_subdirectory(cmake)
endif()
if(SAMPGDK_BUILD_DOCS AND SAMPGDK_TOP_LEVEL)
  add_subdirectory(doc)
endif()
add_subdirectory(include)
add_subdirectory(lib)
if(SAMPGDK_BUILD_PLUGINS AND SAMPGDK_TOP_LEVEL)
  add_subdirectory(plugins)
endif()
add_subdirectory(scripts)

# Zydis x86/x64 decoder, pulled in as a git submodule (deps/zydis). Its CMake
# provides the `Zydis` target (static by default) with the right include dirs
# and C11 settings; sampgdk only links against it.
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/deps/zydis ${PROJECT_BINARY_DIR}/deps/zydis)

# sampgdk links Zydis and Zycore as static archives into its own shared
# library; on ELF targets (Linux) the static objects must be compiled as
# position-independent code for the final .so link to succeed.
set_target_properties(Zydis Zycore PROPERTIES POSITION_INDEPENDENT_CODE ON)

sampgdk_get_sources(SAMPGDK_SOURCES)

foreach(file IN LISTS SAMPGDK_SOURCES)
  get_filename_component(extension ${file} EXT)

  if(extension STREQUAL ".idl")
    file(RELATIVE_PATH path ${PROJECT_SOURCE_DIR}/lib/sampgdk ${file})

    get_filename_component(dir ${path} PATH)
    get_filename_component(name ${file} NAME_WE)

    sampgdk_add_api_gen(${name} ${file}
      ${PROJECT_BINARY_DIR}/lib/sampgdk/${dir}/${name}.api)
    sampgdk_add_source_gen(${name} ${file}
      ${PROJECT_BINARY_DIR}/lib/sampgdk/${dir}/${name}.c)
    sampgdk_add_header_gen(${name} ${file}
      ${PROJECT_BINARY_DIR}/include/sampgdk/${dir}/${name}.h)

    list(APPEND SAMPGDK_IDLS ${file})
  endif()
endforeach()

sampgdk_get_sources(SAMPGDK_SOURCES)

foreach(file IN LISTS SAMPGDK_SOURCES)
  get_filename_component(extension ${file} EXT)
  if(extension STREQUAL ".api")
    list(APPEND SAMPGDK_APIS ${file})
  endif()
endforeach()

sampgdk_get_property(SAMPGDK_DEF_FILE SAMPGDK_DEF_FILE)
if(SAMPGDK_DEF_FILE)
  sampgdk_add_exports_gen("def" ${SAMPGDK_DEF_FILE} ${SAMPGDK_APIS})
endif()

sampgdk_get_property(SAMPGDK_SYM_FILE SAMPGDK_SYM_FILE)
if(SAMPGDK_SYM_FILE)
  sampgdk_add_exports_gen("list" ${SAMPGDK_SYM_FILE} ${SAMPGDK_APIS})
endif()

sampgdk_get_sources(SAMPGDK_SOURCES)
sampgdk_get_headers(SAMPGDK_HEADERS)

list(REMOVE_ITEM SAMPGDK_SOURCES ${SAMPGDK_IDLS})

if(SAMPGDK_STATIC)
  add_library(sampgdk STATIC ${SAMPGDK_HEADERS} ${SAMPGDK_SOURCES})
else()
  add_library(sampgdk SHARED ${SAMPGDK_HEADERS} ${SAMPGDK_SOURCES})
endif()

get_target_property(SAMPGDK_OUTPUT_NAME sampgdk OUTPUT_NAME)

if(NOT SAMPGDK_OUTPUT_NAME)
  set(SAMPGDK_OUTPUT_NAME sampgdk)
endif()

if(WIN32)
  set(SAMPGDK_OUTPUT_NAME ${SAMPGDK_OUTPUT_NAME}${SAMPGDK_VERSION_MAJOR})
  if(SAMPGDK_STATIC)
    set(SAMPGDK_OUTPUT_NAME ${SAMPGDK_OUTPUT_NAME}s)
  endif()
endif()

set_target_properties(sampgdk PROPERTIES
  VERSION       ${SAMPGDK_VERSION}
  SOVERSION     ${SAMPGDK_VERSION_MAJOR}
  DEFINE_SYMBOL IN_SAMPGDK
  OUTPUT_NAME   ${SAMPGDK_OUTPUT_NAME}
  DEBUG_POSTFIX d
)

if(SAMPGDK_STATIC)
  target_compile_definitions(sampgdk PUBLIC SAMPGDK_STATIC)
endif()

# Propagate include directories to consumers (via add_subdirectory / link).
# Public headers live partly in the source tree (include/) and partly in the
# build tree (generated sampgdk/*.h), plus the SA-MP plugin SDK headers.
target_include_directories(sampgdk PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_BINARY_DIR}/include
  ${SAMPSDK_INCLUDE_DIR}
  ${SAMPSDK_INCLUDE_DIR}/amx
)

if(WIN32)
  target_compile_definitions(sampgdk PRIVATE WIN32_LEAN_AND_MEAN)
  if(SAMPGDK_ARCH EQUAL 64)
    target_compile_definitions(sampgdk PRIVATE SAMPGDK_64BIT)
  endif()
elseif(UNIX)
  target_compile_definitions(sampgdk PRIVATE _GNU_SOURCE)
endif()

# Zydis is built as a static library by its own subdirectory and linked into
# sampgdk; its `Zydis` target carries the include dirs and C11 setting.
target_link_libraries(sampgdk PRIVATE Zydis)

if(MSVC)
  target_compile_definitions(sampgdk PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX)
  if(SAMPGDK_ARCH EQUAL 32)
    target_compile_options(sampgdk PRIVATE -m32 -fvisibility=hidden)
    if(NOT SAMPGDK_STATIC)
      target_link_libraries(sampgdk PRIVATE
               -m32 -Wl,--no-undefined,--retain-symbols-file=${SAMPGDK_SYM_FILE})
    endif()
  else()
    target_compile_options(sampgdk PRIVATE -m64 -fvisibility=hidden)
    target_compile_definitions(sampgdk PRIVATE SAMPGDK_64BIT)
    if(NOT SAMPGDK_STATIC)
      target_link_libraries(sampgdk PRIVATE
               -m64 -Wl,--no-undefined,--retain-symbols-file=${SAMPGDK_SYM_FILE})
    endif()
  endif()
endif()

if(UNIX)
  sampgdk_add_libraries(${CMAKE_DL_LIBS} rt)
endif()

sampgdk_get_libraries(SAMPGDK_LIBRARIES)

if(SAMPGDK_STATIC)
  target_link_libraries(sampgdk INTERFACE ${SAMPGDK_LIBRARIES})
else()
  target_link_libraries(sampgdk PRIVATE ${SAMPGDK_LIBRARIES})
endif()

add_dependencies(sampgdk sampgdk_init)

if(SAMPGDK_TOP_LEVEL)
  foreach(header IN LISTS SAMPGDK_HEADERS)
    get_filename_component(path ${header} PATH)
    get_filename_component(abs_path ${path} ABSOLUTE)

    string(FIND ${abs_path} ${PROJECT_BINARY_DIR} pos)
    if(pos LESS 0)
      file(RELATIVE_PATH dir ${PROJECT_SOURCE_DIR} ${abs_path})
    else()
      file(RELATIVE_PATH dir ${PROJECT_BINARY_DIR} ${abs_path})
    endif()

    install(FILES ${header} DESTINATION ${dir} COMPONENT dev)
  endforeach()

  install(TARGETS sampgdk RUNTIME DESTINATION bin COMPONENT bin
                          ARCHIVE DESTINATION lib COMPONENT dev
                          LIBRARY DESTINATION lib COMPONENT bin)

endif()

# Packaging (CPack) only makes sense for a standalone build; skip it when
# sampgdk is consumed via add_subdirectory from another project.
if(SAMPGDK_TOP_LEVEL)
  if(SAMPGDK_ARCH EQUAL 32)
    set(ARCH_TAG "32")
  else()
    set(ARCH_TAG "64")
  endif()

  if(WIN32)
    set(CPACK_PACKAGE_FILE_NAME sampgdk-${SAMPGDK_VERSION}-win${ARCH_TAG})
    set(CPACK_GENERATOR ZIP)
  elseif(UNIX)
    set(CPACK_PACKAGE_FILE_NAME sampgdk-${SAMPGDK_VERSION}-linux${ARCH_TAG})
    set(CPACK_GENERATOR TGZ)
  endif()

  if(SAMPGDK_STATIC)
    set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_FILE_NAME}-static)
  endif()

  include(CPack)
endif()
