function(add_mini_test layer module source)
    get_filename_component(test_name "${source}" NAME_WE)
    string(REPLACE "/" "_" module_id "${module}")
    set(target "${layer}_${module_id}_${test_name}")
    set(test_id "${layer}.${module}.${test_name}")

    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE mini_trantor)
    add_test(NAME "${test_id}" COMMAND ${target})
    set_tests_properties("${test_id}" PROPERTIES LABELS "${layer};${module}")

    set_property(GLOBAL APPEND PROPERTY MINI_ALL_TEST_TARGETS ${target})
    if(layer STREQUAL "unit")
        set_property(GLOBAL APPEND PROPERTY MINI_UNIT_TEST_TARGETS ${target})
    elseif(layer STREQUAL "contract")
        set_property(GLOBAL APPEND PROPERTY MINI_CONTRACT_TEST_TARGETS ${target})
    elseif(layer STREQUAL "integration")
        set_property(GLOBAL APPEND PROPERTY MINI_INTEGRATION_TEST_TARGETS ${target})
    endif()
endfunction()

add_mini_test(unit buffer ${CMAKE_CURRENT_SOURCE_DIR}/unit/buffer/test_buffer.cpp)
add_mini_test(unit channel ${CMAKE_CURRENT_SOURCE_DIR}/unit/channel/test_channel.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_task.cpp)
add_mini_test(unit timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/unit/timer_queue/test_timer_id.cpp)

add_mini_test(contract event_loop ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop/test_event_loop.cpp)
add_mini_test(
    contract
    event_loop_thread
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread/test_event_loop_thread.cpp
)
add_mini_test(contract poller ${CMAKE_CURRENT_SOURCE_DIR}/contract/poller/test_poller_contract.cpp)
add_mini_test(contract timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/contract/timer_queue/test_timer_queue.cpp)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_tcp_server.cpp)
add_mini_test(contract tcp_connection ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_connection/test_tcp_connection.cpp)
add_mini_test(
    contract
    event_loop_thread_pool
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp
)

add_mini_test(integration tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server.cpp)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_threaded.cpp
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_idle_timeout.cpp
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_backpressure.cpp
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_backpressure_policy.cpp
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_coroutine_echo_server.cpp
)

get_property(unit_targets GLOBAL PROPERTY MINI_UNIT_TEST_TARGETS)
get_property(contract_targets GLOBAL PROPERTY MINI_CONTRACT_TEST_TARGETS)
get_property(integration_targets GLOBAL PROPERTY MINI_INTEGRATION_TEST_TARGETS)
get_property(all_test_targets GLOBAL PROPERTY MINI_ALL_TEST_TARGETS)

add_custom_target(
    check-unit
    DEPENDS ${unit_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L unit
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-contract
    DEPENDS ${contract_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L contract
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-integration
    DEPENDS ${integration_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L integration
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-tests
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)
