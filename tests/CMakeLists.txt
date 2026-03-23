option(MCP_BUILD_TESTS "Build tests" ON)
if(NOT MCP_BUILD_TESTS)
    return()
endif()

find_package(Boost REQUIRED COMPONENTS unit_test_framework)

function(add_seastar_unit_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE
        mcp_sdk::mcp_sdk
        Seastar::seastar
        Seastar::seastar_testing
        Boost::unit_test_framework)
    target_compile_definitions(${name} PRIVATE SEASTAR_TESTING_MAIN BOOST_TEST_DYN_LINK)
    add_test(NAME ${name}
        COMMAND ${name} --log_level=test_suite --catch_system_errors=no
                        -- -c 1 -m 256M --overprovisioned
                           --default-log-level=error)
endfunction()

add_seastar_unit_test(test_dispatcher unit/test_dispatcher.cc)
add_seastar_unit_test(test_registry   unit/test_registry.cc)
