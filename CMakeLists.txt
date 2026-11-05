cmake_minimum_required(VERSION 3.22)
set(SC_MODULE lxd)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-lxd" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_LXD_BUILD_TESTS "Build simply-cpp-lxd tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# nlohmann/json and the rest of sc-core.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.25.0)
# sc::rest (rest.h), sc-core's until 2.0.0.
find_or_install_package(sc-http simply-cpp-http-dev simply-cpp-http 1.1.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(sc_lxd
        LINK_LIBRARIES sc::sc-core sc::sc-http-shared
        PUBLIC_LINK_LIBRARIES sc::sc-core sc::sc-http-shared)

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

# sc-core is built into the library and the demo, so the runtime package doesn't need simply-cpp;
# sc-http's shared library it does.
package_sc_module(DEPENDS "simply-cpp-http (>= 1.1.0~)" DEV_DEPENDS "simply-cpp-dev (>= 1.25.0~)" "simply-cpp-http-dev (>= 1.1.0~)")
