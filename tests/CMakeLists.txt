function(add_mini_test layer module source)
    get_filename_component(test_name "${source}" NAME_WE)
    string(REPLACE "/" "_" module_id "${module}")
    set(target "${layer}_${module_id}_${test_name}")
    set(test_id "${layer}.${module}.${test_name}")
    set(test_labels ${layer} ${module} ${ARGN})

    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE mini_trantor)
    # Existing contracts use assert(), including setup with side effects.
    # Keep assertions active in Release too; the library remains an ordinary Release build.
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME "${test_id}" COMMAND ${target})
    set_tests_properties("${test_id}" PROPERTIES LABELS "${test_labels}" TIMEOUT 60)

    set_property(GLOBAL APPEND PROPERTY MINI_ALL_TEST_TARGETS ${target})
    if(layer STREQUAL "unit")
        set_property(GLOBAL APPEND PROPERTY MINI_UNIT_TEST_TARGETS ${target})
    elseif(layer STREQUAL "contract")
        set_property(GLOBAL APPEND PROPERTY MINI_CONTRACT_TEST_TARGETS ${target})
    elseif(layer STREQUAL "integration")
        set_property(GLOBAL APPEND PROPERTY MINI_INTEGRATION_TEST_TARGETS ${target})
    endif()
endfunction()

# Retained contracts run in every applicable platform configuration.
add_mini_test(contract build ${CMAKE_CURRENT_SOURCE_DIR}/contract/build/test_assertions_enabled.cpp)
add_mini_test(contract event_loop ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop/test_active_channel_removal.cpp lifecycle)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_connection_event_contract.cpp threading lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_task_lifetime.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_suspend_publication.cpp coro lifecycle threading)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_tcp_awaitable_lifetime.cpp coro lifecycle threading)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_task_start_contract.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_combinator_lifetime.cpp coro lifecycle threading)
add_mini_test(contract dns ${CMAKE_CURRENT_SOURCE_DIR}/contract/dns/test_dns_lifetime.cpp lifecycle threading)
add_mini_test(contract event_loop ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop/test_loop_handle.cpp lifecycle threading)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_resolve_awaitable_lifetime.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_cancel_during_teardown.cpp coro lifecycle threading)
add_mini_test(contract event_loop_thread ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread/test_start_stop_failure.cpp lifecycle threading)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_cancellation_reentry.cpp lifecycle coro)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_close_during_destruction.cpp lifecycle threading)
add_mini_test(contract connector ${CMAKE_CURRENT_SOURCE_DIR}/contract/connector/test_connector_reentry.cpp lifecycle threading)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_socket_creation.cpp lifecycle)

add_test(NAME contract.build.install_consumer
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DPROJECT_BINARY_DIR=${PROJECT_BINARY_DIR}
        -DCONFIG=$<CONFIG>
        -DTLS_ENABLED=${MINI_ENABLE_TLS}
        -DSANITIZED=${MINI_ENABLE_ASAN_UBSAN}
        -DTSAN_ENABLED=${MINI_ENABLE_TSAN}
        -DGENERATOR=${CMAKE_GENERATOR}
        -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_install.cmake)
set_tests_properties(contract.build.install_consumer PROPERTIES LABELS "contract;build" TIMEOUT 120)

if(WIN32)
    add_mini_test(unit timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/unit/timer_queue/test_timer_id.cpp)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_task.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_sleep_awaitable.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_cancellation_token.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_all.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_any.cpp coro lifecycle)
    add_mini_test(unit framing ${CMAKE_CURRENT_SOURCE_DIR}/unit/framing/test_framer.cpp protocol transport)

    add_mini_test(contract event_loop ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop/test_event_loop.cpp threading lifecycle)
    add_mini_test(
        contract
        event_loop_thread
        ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread/test_event_loop_thread.cpp
        threading
        lifecycle
    )
    add_mini_test(
        contract
        event_loop_thread_pool
        ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp
        threading
        lifecycle
    )
    add_mini_test(contract timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/contract/timer_queue/test_timer_queue.cpp)
    add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_sleep_awaitable.cpp coro lifecycle)
    add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_combinator_contract.cpp coro lifecycle)
    add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_cancellation_contract.cpp coro lifecycle)
    add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_timeout_contract.cpp coro lifecycle)
    add_mini_test(contract framing ${CMAKE_CURRENT_SOURCE_DIR}/contract/framing/test_framer_contract.cpp protocol transport)

else()

add_mini_test(unit buffer ${CMAKE_CURRENT_SOURCE_DIR}/unit/buffer/test_buffer.cpp)

add_mini_test(unit channel ${CMAKE_CURRENT_SOURCE_DIR}/unit/channel/test_channel.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_task.cpp coro lifecycle)
add_mini_test(unit timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/unit/timer_queue/test_timer_id.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_sleep_awaitable.cpp coro lifecycle)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_cancellation_token.cpp coro lifecycle)
add_mini_test(unit connector ${CMAKE_CURRENT_SOURCE_DIR}/unit/connector/test_connector.cpp)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_all.cpp coro lifecycle)
add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_any.cpp coro lifecycle)
add_mini_test(unit dns ${CMAKE_CURRENT_SOURCE_DIR}/unit/dns/test_dns_resolver.cpp)
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_connection_awaiter_registry.cpp coro lifecycle)
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_connection_backpressure_controller.cpp)
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_connection_transport.cpp transport lifecycle)

add_mini_test(unit framing ${CMAKE_CURRENT_SOURCE_DIR}/unit/framing/test_framer.cpp protocol transport)
add_mini_test(unit acceptor ${CMAKE_CURRENT_SOURCE_DIR}/unit/acceptor/test_acceptor_stop.cpp lifecycle)
add_mini_test(unit event_loop_thread_pool ${CMAKE_CURRENT_SOURCE_DIR}/unit/event_loop_thread_pool/test_thread_pool_stop.cpp threading lifecycle)

add_mini_test(contract acceptor ${CMAKE_CURRENT_SOURCE_DIR}/contract/acceptor/test_acceptor.cpp)
add_mini_test(contract connector ${CMAKE_CURRENT_SOURCE_DIR}/contract/connector/test_connector.cpp)
add_mini_test(contract buffer ${CMAKE_CURRENT_SOURCE_DIR}/contract/buffer/test_buffer_contract.cpp)
add_mini_test(contract channel ${CMAKE_CURRENT_SOURCE_DIR}/contract/channel/test_channel_contract.cpp)
add_mini_test(contract event_loop ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop/test_event_loop.cpp threading lifecycle)
add_mini_test(
    contract
    event_loop_thread
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread/test_event_loop_thread.cpp
    threading
    lifecycle
)
add_mini_test(contract poller ${CMAKE_CURRENT_SOURCE_DIR}/contract/poller/test_poller_contract.cpp)
add_mini_test(contract timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/contract/timer_queue/test_timer_queue.cpp)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_tcp_server.cpp threading lifecycle)
add_mini_test(contract tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_server/test_shutdown_ordering.cpp lifecycle)

add_mini_test(contract framing ${CMAKE_CURRENT_SOURCE_DIR}/contract/framing/test_framer_contract.cpp protocol transport)
add_mini_test(contract tcp_connection ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_connection/test_tcp_connection.cpp threading lifecycle coro)
add_mini_test(
    contract
    event_loop_thread_pool
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp
    threading
    lifecycle
)
add_mini_test(contract tcp_client ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_client/test_tcp_client.cpp)
add_mini_test(contract tcp_client ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_client/test_dns_candidates.cpp lifecycle threading)
# Compile the same production resolver in this executable so ld --wrap covers
# its OS boundary for both static and shared library builds. No runtime test hook.
target_sources(contract_tcp_client_test_dns_candidates PRIVATE
    ${PROJECT_SOURCE_DIR}/mini/net/DnsResolver.cc
    ${PROJECT_SOURCE_DIR}/mini/net/platform/SocketsOps_linux.cc)
target_link_options(contract_tcp_client_test_dns_candidates PRIVATE
    -Wl,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=socket)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_sleep_awaitable.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_combinator_contract.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_cancellation_contract.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_timeout_contract.cpp coro lifecycle)
add_mini_test(contract dns ${CMAKE_CURRENT_SOURCE_DIR}/contract/dns/test_dns_contract.cpp)
add_mini_test(contract signal ${CMAKE_CURRENT_SOURCE_DIR}/contract/signal/test_signal_handling.cpp lifecycle)

add_mini_test(integration tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server.cpp)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_threaded.cpp
    threading
    lifecycle
)

add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_idle_timeout.cpp
    lifecycle
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
    lifecycle
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_coroutine_echo_server.cpp
    coro
    lifecycle
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_coroutine_idle_timeout.cpp
    coro
    lifecycle
)
add_mini_test(
    integration
    coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/coroutine/test_timeout_race.cpp
    coro
    lifecycle
    threading
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

if(MINI_ENABLE_TLS)
# --- TLS tests ---
add_mini_test(unit tls ${CMAKE_CURRENT_SOURCE_DIR}/unit/tls/test_tls_context.cpp protocol)

add_mini_test(contract tls ${CMAKE_CURRENT_SOURCE_DIR}/contract/tls/test_tls_handshake.cpp protocol lifecycle)
add_mini_test(integration tls ${CMAKE_CURRENT_SOURCE_DIR}/integration/tls/test_tls_echo.cpp protocol lifecycle)

# TLS tests need SOURCE_DIR to locate test certificates
foreach(tls_target unit_tls_test_tls_context contract_tls_test_tls_handshake integration_tls_test_tls_echo)
    target_compile_definitions(${tls_target} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endforeach()
endif()







# --- IPv6 dual-stack tests (v5-gamma) ---
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_inet_address_ipv6.cpp)
add_mini_test(contract tcp_client ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_client/test_tcp_client_ipv6.cpp)
add_mini_test(integration tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_ipv6.cpp)

# --- Configuration and observability tests (v5-delta) ---
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_options_contract.cpp)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_metrics_hook_contract.cpp)

endif() # platform test selection

get_property(unit_targets GLOBAL PROPERTY MINI_UNIT_TEST_TARGETS)
get_property(contract_targets GLOBAL PROPERTY MINI_CONTRACT_TEST_TARGETS)
get_property(integration_targets GLOBAL PROPERTY MINI_INTEGRATION_TEST_TARGETS)
get_property(all_test_targets GLOBAL PROPERTY MINI_ALL_TEST_TARGETS)

add_custom_target(
    check-unit
    DEPENDS ${unit_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure -L unit
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-contract
    DEPENDS ${contract_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure -L contract
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-integration
    DEPENDS ${integration_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure -L integration
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-tests
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-lifecycle
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure -L lifecycle
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-threading
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure -L threading
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-protocol
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure -L protocol
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)
