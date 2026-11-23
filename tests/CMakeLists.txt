set(SC_TEST_LINK_LIBRARIES sc-lxd)

add_sc_test(instances LABELS unit)
add_sc_test(client LABELS unit)
target_compile_definitions(test-client PRIVATE SC_LXD_TEST_RESOURCE="${CMAKE_CURRENT_SOURCE_DIR}/resource")
target_compile_definitions(test-instances PRIVATE SC_LXD_TEST_RESOURCE="${CMAKE_CURRENT_SOURCE_DIR}/resource")
add_sc_test(standalone LABELS unit)
target_compile_definitions(test-standalone PRIVATE SC_LXD_TEST_RESOURCE="${CMAKE_CURRENT_SOURCE_DIR}/resource")
