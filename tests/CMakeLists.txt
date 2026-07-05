function(add_mini_test layer module source)
    get_filename_component(test_name "${source}" NAME_WE)
    string(REPLACE "/" "_" module_id "${module}")
    set(target "${layer}_${module_id}_${test_name}")
    set(test_id "${layer}.${module}.${test_name}")
    set(test_labels ${layer} ${module} ${ARGN})

    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE mini_trantor)
    add_test(NAME "${test_id}" COMMAND ${target})
    set_tests_properties("${test_id}" PROPERTIES LABELS "${test_labels}")

    set_property(GLOBAL APPEND PROPERTY MINI_ALL_TEST_TARGETS ${target})
    if(layer STREQUAL "unit")
        set_property(GLOBAL APPEND PROPERTY MINI_UNIT_TEST_TARGETS ${target})
    elseif(layer STREQUAL "contract")
        set_property(GLOBAL APPEND PROPERTY MINI_CONTRACT_TEST_TARGETS ${target})
    elseif(layer STREQUAL "integration")
        set_property(GLOBAL APPEND PROPERTY MINI_INTEGRATION_TEST_TARGETS ${target})
    endif()
endfunction()

# Scope labels such as transport-experimental/kcp-preview/pmtu-preview mark
# preview boundaries only; they must not be used to hide failing foundation tests.

if(WIN32)
    add_mini_test(unit timer_queue ${CMAKE_CURRENT_SOURCE_DIR}/unit/timer_queue/test_timer_id.cpp)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_task.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_sleep_awaitable.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_cancellation_token.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_all.cpp coro lifecycle)
    add_mini_test(unit coroutine ${CMAKE_CURRENT_SOURCE_DIR}/unit/coroutine/test_when_any.cpp coro lifecycle)
    add_mini_test(unit framing ${CMAKE_CURRENT_SOURCE_DIR}/unit/framing/test_framer.cpp protocol transport)
    add_mini_test(unit kcp ${CMAKE_CURRENT_SOURCE_DIR}/unit/kcp/test_kcp_codec.cpp protocol transport transport-experimental kcp-preview)
    add_mini_test(unit game ${CMAKE_CURRENT_SOURCE_DIR}/unit/game/test_player_session_fsm.cpp lifecycle)
    add_mini_test(unit game ${CMAKE_CURRENT_SOURCE_DIR}/unit/game/test_game_backpressure_policy.cpp metrics)
    add_mini_test(unit game ${CMAKE_CURRENT_SOURCE_DIR}/unit/game/test_game_gateway_security_policy.cpp protocol)
    add_mini_test(unit logic ${CMAKE_CURRENT_SOURCE_DIR}/unit/logic/test_game_command_queue.cpp threading)
    add_mini_test(unit metrics ${CMAKE_CURRENT_SOURCE_DIR}/unit/metrics/test_metrics_hook_ext.cpp)
    add_mini_test(unit metrics ${CMAKE_CURRENT_SOURCE_DIR}/unit/metrics/test_metrics_exporter.cpp metrics)

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
    add_mini_test(contract game ${CMAKE_CURRENT_SOURCE_DIR}/contract/game/test_session_manager_contract.cpp lifecycle)
    add_mini_test(contract game ${CMAKE_CURRENT_SOURCE_DIR}/contract/game/test_game_backpressure_policy_contract.cpp metrics)
    add_mini_test(contract game ${CMAKE_CURRENT_SOURCE_DIR}/contract/game/test_game_gateway_security_contract.cpp protocol lifecycle)
    add_mini_test(contract logic ${CMAKE_CURRENT_SOURCE_DIR}/contract/logic/test_logic_loop_timing_contract.cpp threading)
    return()
endif()

add_mini_test(unit buffer ${CMAKE_CURRENT_SOURCE_DIR}/unit/buffer/test_buffer.cpp)
add_mini_test(unit buffer ${CMAKE_CURRENT_SOURCE_DIR}/unit/buffer/test_payload_pool.cpp)
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
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_protocol_connection_adapter.cpp protocol lifecycle)
add_mini_test(unit transport ${CMAKE_CURRENT_SOURCE_DIR}/unit/transport/test_transport_abstraction.cpp transport)
add_mini_test(unit transport ${CMAKE_CURRENT_SOURCE_DIR}/unit/transport/test_path_mtu_cache.cpp transport transport-experimental pmtu-preview)
add_mini_test(unit game ${CMAKE_CURRENT_SOURCE_DIR}/unit/game/test_player_session_fsm.cpp lifecycle)
add_mini_test(unit game ${CMAKE_CURRENT_SOURCE_DIR}/unit/game/test_game_backpressure_policy.cpp metrics)
add_mini_test(unit game ${CMAKE_CURRENT_SOURCE_DIR}/unit/game/test_game_gateway_security_policy.cpp protocol)
add_mini_test(unit logic ${CMAKE_CURRENT_SOURCE_DIR}/unit/logic/test_game_command_queue.cpp threading)
add_mini_test(unit kcp ${CMAKE_CURRENT_SOURCE_DIR}/unit/kcp/test_kcp_codec.cpp protocol transport transport-experimental kcp-preview)
add_mini_test(unit framing ${CMAKE_CURRENT_SOURCE_DIR}/unit/framing/test_framer.cpp protocol transport)
add_mini_test(unit acceptor ${CMAKE_CURRENT_SOURCE_DIR}/unit/acceptor/test_acceptor_stop.cpp lifecycle)
add_mini_test(unit event_loop_thread_pool ${CMAKE_CURRENT_SOURCE_DIR}/unit/event_loop_thread_pool/test_thread_pool_stop.cpp threading lifecycle)
add_mini_test(unit broadcast ${CMAKE_CURRENT_SOURCE_DIR}/unit/broadcast/test_dispatcher_batch.cpp threading)
add_mini_test(unit metrics ${CMAKE_CURRENT_SOURCE_DIR}/unit/metrics/test_metrics_hook_ext.cpp)
add_mini_test(unit metrics ${CMAKE_CURRENT_SOURCE_DIR}/unit/metrics/test_metrics_exporter.cpp metrics)

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
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_tcp_server_broadcast_router_contract.cpp threading)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_tcp_server_broadcast_admission_contract.cpp threading)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_broadcast_batch_contract.cpp threading)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_metrics_exporter_contract.cpp metrics threading)
add_mini_test(contract broadcast ${CMAKE_CURRENT_SOURCE_DIR}/contract/broadcast/test_payload_sharing_contract.cpp threading)
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
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_sleep_awaitable.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_combinator_contract.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_cancellation_contract.cpp coro lifecycle)
add_mini_test(contract coroutine ${CMAKE_CURRENT_SOURCE_DIR}/contract/coroutine/test_timeout_contract.cpp coro lifecycle)
add_mini_test(contract dns ${CMAKE_CURRENT_SOURCE_DIR}/contract/dns/test_dns_contract.cpp)
add_mini_test(contract signal ${CMAKE_CURRENT_SOURCE_DIR}/contract/signal/test_signal_handling.cpp lifecycle)
add_mini_test(contract transport ${CMAKE_CURRENT_SOURCE_DIR}/contract/transport/test_transport_contract.cpp transport lifecycle)
add_mini_test(
    contract
    kcp
    ${CMAKE_CURRENT_SOURCE_DIR}/contract/kcp/test_kcp_transport_stress_contract.cpp
    protocol
    transport
    transport-experimental
    kcp-preview
    pmtu-preview
    threading
    lifecycle
)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_broadcast_threaded.cpp
    threading
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_broadcast_batch.cpp
    threading
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_broadcast_ordered_stress.cpp
    threading
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_broadcast_stress_reconnect.cpp
    threading
    lifecycle
)
add_mini_test(
    integration
    tcp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_broadcast_session_group.cpp
    threading
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

# --- TLS tests ---
add_mini_test(unit tls ${CMAKE_CURRENT_SOURCE_DIR}/unit/tls/test_tls_context.cpp protocol)
add_mini_test(unit udp ${CMAKE_CURRENT_SOURCE_DIR}/unit/udp/test_udp_socket.cpp transport pmtu-preview)
add_mini_test(unit udp ${CMAKE_CURRENT_SOURCE_DIR}/unit/udp/test_icmp_path_mtu_listener.cpp transport protocol transport-experimental pmtu-preview)
add_mini_test(unit udp ${CMAKE_CURRENT_SOURCE_DIR}/unit/udp/test_path_mtu_signal_adapter.cpp transport protocol transport-experimental pmtu-preview)
add_mini_test(contract tls ${CMAKE_CURRENT_SOURCE_DIR}/contract/tls/test_tls_handshake.cpp protocol lifecycle)
add_mini_test(integration tls ${CMAKE_CURRENT_SOURCE_DIR}/integration/tls/test_tls_echo.cpp protocol lifecycle)

# TLS tests need SOURCE_DIR to locate test certificates
foreach(tls_target unit_tls_test_tls_context contract_tls_test_tls_handshake integration_tls_test_tls_echo)
    target_compile_definitions(${tls_target} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endforeach()

# --- HTTP tests ---
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_request.cpp protocol)
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_response.cpp protocol)
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_context.cpp protocol)
add_mini_test(unit http ${CMAKE_CURRENT_SOURCE_DIR}/unit/http/test_http_response_context.cpp protocol)
add_mini_test(contract http ${CMAKE_CURRENT_SOURCE_DIR}/contract/http/test_http_server.cpp protocol lifecycle)
add_mini_test(contract http ${CMAKE_CURRENT_SOURCE_DIR}/contract/http/test_http_transport_contract.cpp protocol lifecycle)
add_mini_test(contract http ${CMAKE_CURRENT_SOURCE_DIR}/contract/http/test_http_client.cpp protocol lifecycle)
add_mini_test(integration http ${CMAKE_CURRENT_SOURCE_DIR}/integration/http/test_http_server.cpp protocol lifecycle)
add_mini_test(integration http ${CMAKE_CURRENT_SOURCE_DIR}/integration/http/test_http_client.cpp protocol lifecycle)

# --- WebSocket tests ---
add_mini_test(unit ws ${CMAKE_CURRENT_SOURCE_DIR}/unit/ws/test_ws_codec.cpp protocol)
add_mini_test(unit ws ${CMAKE_CURRENT_SOURCE_DIR}/unit/ws/test_ws_handshake.cpp protocol)
add_mini_test(contract ws ${CMAKE_CURRENT_SOURCE_DIR}/contract/ws/test_ws_server.cpp protocol lifecycle)
add_mini_test(integration ws ${CMAKE_CURRENT_SOURCE_DIR}/integration/ws/test_ws_server.cpp protocol lifecycle)

add_mini_test(contract udp ${CMAKE_CURRENT_SOURCE_DIR}/contract/udp/test_udp_server_contract.cpp transport lifecycle)
add_mini_test(contract game ${CMAKE_CURRENT_SOURCE_DIR}/contract/game/test_session_manager_contract.cpp lifecycle)
add_mini_test(contract game ${CMAKE_CURRENT_SOURCE_DIR}/contract/game/test_game_backpressure_policy_contract.cpp metrics)
add_mini_test(contract game ${CMAKE_CURRENT_SOURCE_DIR}/contract/game/test_game_gateway_security_contract.cpp protocol lifecycle)
add_mini_test(contract logic ${CMAKE_CURRENT_SOURCE_DIR}/contract/logic/test_logic_loop_timing_contract.cpp threading)
add_mini_test(integration udp ${CMAKE_CURRENT_SOURCE_DIR}/integration/udp/test_udp_loopback.cpp transport)
add_mini_test(integration udp ${CMAKE_CURRENT_SOURCE_DIR}/integration/udp/test_udp_sendto_stop_lifecycle.cpp transport lifecycle)
add_mini_test(integration kcp ${CMAKE_CURRENT_SOURCE_DIR}/integration/kcp/test_kcp_reliable_flow.cpp protocol transport transport-experimental kcp-preview)

# --- RPC tests ---
add_mini_test(unit rpc ${CMAKE_CURRENT_SOURCE_DIR}/unit/rpc/test_rpc_codec.cpp protocol)
add_mini_test(unit codec ${CMAKE_CURRENT_SOURCE_DIR}/unit/codec/test_protobuf_adapter.cpp protocol)
add_mini_test(unit codec ${CMAKE_CURRENT_SOURCE_DIR}/unit/codec/test_flatbuffers_adapter.cpp protocol)
add_mini_test(contract rpc ${CMAKE_CURRENT_SOURCE_DIR}/contract/rpc/test_rpc_server.cpp protocol lifecycle)
add_mini_test(contract codec ${CMAKE_CURRENT_SOURCE_DIR}/contract/codec/test_codec_adapter_contract.cpp protocol)
add_mini_test(contract rpc ${CMAKE_CURRENT_SOURCE_DIR}/contract/rpc/test_rpc_client_pool.cpp protocol lifecycle)
add_mini_test(integration rpc ${CMAKE_CURRENT_SOURCE_DIR}/integration/rpc/test_rpc.cpp protocol lifecycle)
add_mini_test(integration codec ${CMAKE_CURRENT_SOURCE_DIR}/integration/codec/test_game_message_roundtrip.cpp protocol)
add_mini_test(integration transport ${CMAKE_CURRENT_SOURCE_DIR}/integration/transport/test_framing_halfpack.cpp protocol transport)
add_mini_test(integration transport ${CMAKE_CURRENT_SOURCE_DIR}/integration/transport/test_transport_adapter_loopback.cpp transport lifecycle)
add_mini_test(integration game ${CMAKE_CURRENT_SOURCE_DIR}/integration/game/test_connect_auth_replay.cpp lifecycle protocol)
add_mini_test(integration game ${CMAKE_CURRENT_SOURCE_DIR}/integration/game/test_reconnect_flow.cpp lifecycle threading)
add_mini_test(integration game ${CMAKE_CURRENT_SOURCE_DIR}/integration/game/test_game_server_vertical_slice.cpp protocol threading lifecycle)
add_mini_test(integration game ${CMAKE_CURRENT_SOURCE_DIR}/integration/game/test_game_server_handoff_contract.cpp protocol threading lifecycle metrics)
add_mini_test(integration game ${CMAKE_CURRENT_SOURCE_DIR}/integration/game/test_game_backpressure_policy.cpp protocol threading lifecycle metrics)
add_mini_test(integration game ${CMAKE_CURRENT_SOURCE_DIR}/integration/game/test_game_gateway_security.cpp protocol threading lifecycle metrics)
add_mini_test(integration logic ${CMAKE_CURRENT_SOURCE_DIR}/integration/logic/test_network_to_logic_roundtrip.cpp threading)

# --- IPv6 dual-stack tests (v5-gamma) ---
add_mini_test(unit net ${CMAKE_CURRENT_SOURCE_DIR}/unit/net/test_inet_address_ipv6.cpp)
add_mini_test(contract tcp_client ${CMAKE_CURRENT_SOURCE_DIR}/contract/tcp_client/test_tcp_client_ipv6.cpp)
add_mini_test(integration tcp_server ${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_server/test_tcp_server_ipv6.cpp)

# --- Configuration and observability tests (v5-delta) ---
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_options_contract.cpp)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_metrics_hook_contract.cpp)
add_mini_test(contract net ${CMAKE_CURRENT_SOURCE_DIR}/contract/net/test_game_metrics_contract.cpp)
add_mini_test(integration benchmark ${CMAKE_CURRENT_SOURCE_DIR}/integration/benchmark/test_fps_like_broadcast_latency.cpp benchmark threading)
add_mini_test(integration benchmark ${CMAKE_CURRENT_SOURCE_DIR}/integration/benchmark/test_game_server_metrics_smoke.cpp benchmark threading lifecycle)

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

add_custom_target(
    check-lifecycle
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L lifecycle
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-threading
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L threading
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-protocol
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L protocol
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(
    check-benchmark
    DEPENDS ${all_test_targets}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L benchmark
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)
