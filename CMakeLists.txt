cmake_minimum_required(VERSION 3.22)
set(SC_MODULE lxd)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-lxd" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_LXD_BUILD_TESTS "Build simply-cpp-lxd tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::rest with client certificates, trust() and pinning since 1.22.0, del() since 1.23.0, put() since 1.25.0 (over libcurl),
# and nlohmann/json.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.25.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(sc_lxd
        LINK_LIBRARIES sc::sc-core
        PUBLIC_LINK_LIBRARIES sc::sc-core)

add_sc_libraries(DESCRIPTION "simply-cpp LXD: a cluster's instances and their state over LXD's REST API")
target_compile_features(sc-lxd PUBLIC cxx_std_20)
target_compile_features(sc-lxd-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-lxd
        COMPONENT development)

if (SC_LXD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-lxd-test)
endif ()

add_sc_example(sc-lxd-demo)

# sc-core is built into the library and the demo, so the runtime package doesn't need simply-cpp.
package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.25.0~)")
