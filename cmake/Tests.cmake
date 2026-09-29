set(_satview_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB _satview_test_sources CONFIGURE_DEPENDS
    "${_satview_root}/tests/satview_*_tests.cpp")
if(APPLE)
    list(APPEND _satview_test_sources
        "${_satview_root}/tests/satview_metal_texture_refresh_tests.mm")
    set_source_files_properties(
        "${_satview_root}/tests/satview_metal_texture_refresh_tests.mm"
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

draxul_add_test_target(
    draxul-test-satview satview 2 ${_satview_test_sources})
target_link_libraries(draxul-test-satview PRIVATE
    draxul-satview-core-test-internals
    draxul-satview-services-test-internals
    draxul-satview-runtime-test-internals
    draxul-satview-renderer-test-internals
    Draxul::PluginSupport::Adapter
    Draxul::PluginSupport::Config
    SDL3::SDL3
    draxul-host-api
    draxul-renderer)
target_compile_definitions(draxul-test-satview PRIVATE
    DRAXUL_ENABLE_SATVIEW
    "DRAXUL_SATVIEW_TEST_BUILD_ROOT=\"${CMAKE_BINARY_DIR}\""
    "DRAXUL_SATVIEW_TEST_ASSET_ROOT=\"${_satview_root}/assets\"")

add_test(
    NAME draxul-satview-catalog-py-tests
    COMMAND ${Python3_EXECUTABLE} -m unittest satview_catalog_py_tests)
set_tests_properties(draxul-satview-catalog-py-tests PROPERTIES
    LABELS "unit;satview;python"
    TIMEOUT 120
    WORKING_DIRECTORY "${_satview_root}/tests")
