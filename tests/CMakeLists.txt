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
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_sleep_awaitable.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_cancellation_token.cpp)
add_mini_test(unit connector ${CMAKE_CURRENT_SOURCE_DIR}/unit/connector/test_connector.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_all.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_any.cpp)
add_mini_test(unit dns ${CMAKE_CURRENT_SOURCE_DIR}/unit/dns/test_dns_resolver.cpp)
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_connection_awaiter_registry.cpp)
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_connection_backpressure_controller.cpp)
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_connection_transport.cpp)
add_mini_test(unit acceptor ${CMAKE_CURRENT_SOURCE_DIR}/unit/acceptor/test_acceptor_stop.cpp)
add_mini_test(unit event_loop_thread_pool ${CMAKE_CURRENT_SOURCE_DIR}/unit/event_loop_thread_pool/test_thread_pool_stop.cpp)

add_mini_test(contract acceptor ${CMAKE_CURRENT_SOURCE_DIR}/contract/acceptor/test_acceptor.cpp)
add_mini_test(contract connector ${CMAKE_CURRENT_SOURCE_DIR}/contract/connector/test_connector.cpp)
add_mini_test(contract buffer ${CMAKE_CURRENT_SOURCE_DIR}/contract/buffer/test_buffer_contract.cpp)
add_mini_test(contract channel ${CMAKE_CURRENT_SOURCE_DIR}/contract/channel/test_channel_contract.cpp)
add_mini_test(contract event_loop ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop/test_event_loop.cpp)
add_mini_test(
    contract
    event_loop_thread
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread/test_event_loop_thread.cpp
)
add_mini_test(contract poller ${CMAKE_CURRENT_SOURCE_DIR}/contract/poller/test_poller_contract.cpp)
add_mini_test(contract timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/contract/timer_queue/test_timer_queue.cpp)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_tcp_server.cpp)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_shutdown_ordering.cpp)
add_mini_test(contract tcp_connection ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_connection/test_tcp_connection.cpp)
add_mini_test(
    contract
    event_loop_thread_pool
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp
)
add_mini_test(contract tcp_client ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_client/test_tcp_client.cpp)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_sleep_awaitable.cpp)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_combinator_contract.cpp)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_cancellation_contract.cpp)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_timeout_contract.cpp)
add_mini_test(contract dns ${CMAKE_CURRENT_SOURCE_DIR}/contract/dns/test_dns_contract.cpp)
add_mini_test(contract signal ${CMAKE_CURRENT_SOURCE_DIR}/contract/signal/test_signal_handling.cpp)

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
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_graceful_shutdown.cpp
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_coroutine_echo_server.cpp
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_coroutine_idle_timeout.cpp
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_timeout_race.cpp
)
add_mini_test(
    integration
    timer_queue
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/timer_queue/test_timer_queue_integration.cpp
)
add_mini_test(
    integration
    tcp_client
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_client/test_tcp_client.cpp
)
add_mini_test(
    integration
    dns
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/dns/test_dns_client.cpp
)

# --- TLS tests ---
add_mini_test(unit tls ${CMAKE_CURRENT_SOURCE_DIR}/unit/tls/test_tls_context.cpp)
add_mini_test(contract tls ${CMAKE_CURRENT_SOURCE_DIR}/contract/tls/test_tls_handshake.cpp)
add_mini_test(integration tls ${CMAKE_CURRENT_SOURCE_DIR}/integration/tls/test_tls_echo.cpp)

# TLS tests need SOURCE_DIR to locate test certificates
foreach(tls_target unit_tls_test_tls_context contract_tls_test_tls_handshake integration_tls_test_tls_echo)
    target_compile_definitions(${tls_target} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endforeach()

# --- HTTP tests ---
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_request.cpp)
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_response.cpp)
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_context.cpp)
add_mini_test(contract http ${CMAKE_CURRENT_SOURCE_DIR}/contract/http/test_http_server.cpp)
add_mini_test(integration http ${CMAKE_CURRENT_SOURCE_DIR}/integration/http/test_http_server.cpp)

# --- WebSocket tests ---
add_mini_test(unit ws ${CMAKE_CURRENT_SOURCE_DIR}/unit/ws/test_ws_codec.cpp)
add_mini_test(unit ws ${CMAKE_CURRENT_SOURCE_DIR}/unit/ws/test_ws_handshake.cpp)
add_mini_test(contract ws ${CMAKE_CURRENT_SOURCE_DIR}/contract/ws/test_ws_server.cpp)
add_mini_test(integration ws ${CMAKE_CURRENT_SOURCE_DIR}/integration/ws/test_ws_server.cpp)

# --- RPC tests ---
add_mini_test(unit rpc ${CMAKE_CURRENT_SOURCE_DIR}/unit/rpc/test_rpc_codec.cpp)
add_mini_test(contract rpc ${CMAKE_CURRENT_SOURCE_DIR}/contract/rpc/test_rpc_server.cpp)
add_mini_test(integration rpc ${CMAKE_CURRENT_SOURCE_DIR}/integration/rpc/test_rpc.cpp)

# --- IPv6 dual-stack tests (v5-gamma) ---
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_inet_address_ipv6.cpp)
add_mini_test(contract tcp_client ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_client/test_tcp_client_ipv6.cpp)
add_mini_test(integration tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_ipv6.cpp)

# --- Configuration and observability tests (v5-delta) ---
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_options_contract.cpp)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_metrics_hook_contract.cpp)

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
