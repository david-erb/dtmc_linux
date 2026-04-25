# source discovery (safe across normal configure + script/find-package modes)
set(DTMC_LINUX_TESTS_GLOB_PATTERNS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_*.c
)

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE AND NOT DEFINED CMAKE_FIND_PACKAGE_NAME)
    file(GLOB DTMC_LINUX_TESTS_SOURCES
        CONFIGURE_DEPENDS
        ${DTMC_LINUX_TESTS_GLOB_PATTERNS}
    )
else()
    # Script mode / find-package mode: CONFIGURE_DEPENDS is not allowed.
    file(GLOB DTMC_LINUX_TESTS_SOURCES
        ${DTMC_LINUX_TESTS_GLOB_PATTERNS}
    )
endif()

add_library(dtmc_linux_tests STATIC
    ${DTMC_LINUX_TESTS_SOURCES}
)

target_include_directories(dtmc_linux_tests
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dtmc_linux_tests
    PUBLIC
        dtmc_base_tests
        dtmc_linux_library
        dtcore_library
)
