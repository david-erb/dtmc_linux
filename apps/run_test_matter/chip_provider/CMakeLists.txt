cmake_minimum_required(VERSION 3.20)
project(chip_provider NONE)

# ---- Inputs ---------------------------------------------------------------
set(CHIP_ROOT "$ENV{CHIP_ROOT}" CACHE PATH "Path to connectedhomeip (Matter) repo")
set(CHIP_BUILD_ARTIFACTS "$ENV{CHIP_BUILD_ARTIFACTS}" CACHE PATH "Chip build artifacts directory.")
set(CHIP_GEN_DIR "${CHIP_BUILD_ARTIFACTS}/linux-x64-chip-tool/gen" CACHE PATH "Directory with generated CHIP files")
set(CHIP_CORE_LIB_DIR "${CHIP_BUILD_ARTIFACTS}/linux-x64-chip-tool/lib" CACHE PATH "Directory with prebuilt core CHIP static libs (.a)")
set(CHIP_APPSERVER_LIB_DIR "${CHIP_BUILD_ARTIFACTS}/linux-x64-chip-tool/obj/third_party/connectedhomeip/src/app/server/lib" CACHE PATH "Directory with prebuilt CHIP static libs (.a)")

# ---- Options --------------------------------------------------------------
option(CHIP_SYSTEM_CONFIG_USE_SOCKETS "Use CHIP System sockets backend" ON)

# ---- Paths ---------------------------------------------------------------
set(_CHIP_CORE_LIB "${CHIP_CORE_LIB_DIR}/libCHIP.a")
set(_CHIP_APPSERVER_LIB "${CHIP_APPSERVER_LIB_DIR}/libCHIPAppServer.a")

# ---- Imported library ----------------------------------------------------
add_library(chip_core STATIC IMPORTED GLOBAL)
set_target_properties(chip_core PROPERTIES
  IMPORTED_LOCATION "${_CHIP_CORE_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES
    "${CHIP_ROOT}/src;${CHIP_ROOT}/third_party/nlassert/repo/include;${CHIP_ROOT}/third_party/nlio/repo/include"
)
target_compile_definitions(chip_core INTERFACE
  $<$<BOOL:${CHIP_SYSTEM_CONFIG_USE_SOCKETS}>:CHIP_SYSTEM_CONFIG_USE_SOCKETS=1>
  CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE=0
)
# ---- Imported library ----------------------------------------------------
add_library(chip_appserver STATIC IMPORTED GLOBAL)
set_target_properties(chip_appserver PROPERTIES
  IMPORTED_LOCATION "${_CHIP_APPSERVER_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES
    "${CHIP_ROOT}/src;${CHIP_ROOT}/third_party/nlassert/repo/include;${CHIP_ROOT}/third_party/nlio/repo/include"
)
target_compile_definitions(chip_appserver INTERFACE
  $<$<BOOL:${CHIP_SYSTEM_CONFIG_USE_SOCKETS}>:CHIP_SYSTEM_CONFIG_USE_SOCKETS=1>
  CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE=0
)

# ---- Interface config -----------------------------------------------------
add_library(chip_config INTERFACE)
target_include_directories(chip_config INTERFACE
  "${CHIP_ROOT}/src"
  "${CHIP_ROOT}/src/include"
  "${CHIP_ROOT}/zzz_generated/app-common"
  "${CHIP_ROOT}/third_party/nlassert/repo/include"
  "${CHIP_ROOT}/third_party/nlio/repo/include"
  "${CHIP_ROOT}/third_party/inipp/repo/inipp"
  "${CHIP_GEN_DIR}/include"
)
target_compile_definitions(chip_config INTERFACE
  $<$<BOOL:${CHIP_SYSTEM_CONFIG_USE_SOCKETS}>:CHIP_SYSTEM_CONFIG_USE_SOCKETS=1>
  CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE=0
)

# ---- Aliases --------------------------------------------------------------
add_library(chip::core ALIAS chip_core)
add_library(chip::appserver ALIAS chip_appserver)
add_library(chip::config ALIAS chip_config)

# ---- GLib dependency ------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED gio-2.0)
target_include_directories(chip_config INTERFACE ${GLIB2_INCLUDE_DIRS})
target_link_libraries(chip_appserver INTERFACE ${GLIB2_LIBRARIES})
target_link_libraries(chip_core INTERFACE ${GLIB2_LIBRARIES})
add_definitions(${GLIB2_CFLAGS_OTHER})
