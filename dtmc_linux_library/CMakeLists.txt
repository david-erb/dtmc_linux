file(GLOB MY_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/dt*.c)

# -----------------------------------------------------------------------------

add_library(dtmc_linux_library STATIC ${MY_SOURCES})
target_include_directories(dtmc_linux_library PUBLIC ${DTMC_LINUX_DIR}/dtmc_linux_library/include)
target_link_libraries(dtmc_linux_library PUBLIC dtmc_base_library)

# --- Matter is enabled? ------------------------------------------------------
if(TARGET chip::config)
    # our source files that refer to Matter
    list(APPEND MY_SOURCES
        ${DTMC_LINUX_DIR}/dtmc_linux_library/src/dtnetportal_matter.cpp
    )

    # Inherit CHIP includes + backend define via INTERFACE from the provider
    target_link_libraries(dtmc_linux_library PUBLIC chip::config chip::appserver)
endif()

# --- pkg-config (used for mosquitto/coap/modbus) -----------------------------
find_package(PkgConfig REQUIRED)

# --- Mosquitto dependency using pkg-config -------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

# Link PUBLIC so apps linking to dtmc_linux_library get mosquitto automatically.
target_link_libraries(dtmc_linux_library
    PUBLIC
        PkgConfig::MOSQUITTO
)

# --- pthreads (mosquitto_loop_start uses a thread; pkg-config usually adds it, ----------
# but be explicit to be safe across distros)
find_package(Threads REQUIRED)
target_link_libraries(dtmc_linux_library
    PUBLIC
        Threads::Threads
)

# --- CoAP dependency using pkg-config -------------------------------
pkg_search_module(COAP REQUIRED IMPORTED_TARGET libcoap-3-openssl)
target_link_libraries(dtmc_linux_library
    PUBLIC
        PkgConfig::COAP
)
message(STATUS "libcoap version: ${COAP_VERSION}")
message(STATUS "libcoap include dirs: ${COAP_INCLUDE_DIRS}")
message(STATUS "libcoap libs: ${COAP_LIBRARIES}")

# --- libmodbus dependency using pkg-config ----------------------------------
pkg_search_module(MODBUS REQUIRED IMPORTED_TARGET libmodbus)

target_link_libraries(dtmc_linux_library
    PUBLIC
        PkgConfig::MODBUS
)

message(STATUS "libmodbus version: ${MODBUS_VERSION}")
message(STATUS "libmodbus include dirs: ${MODBUS_INCLUDE_DIRS}")
message(STATUS "libmodbus libs: ${MODBUS_LIBRARIES}")

# -----------------------------------------------------------------------------
