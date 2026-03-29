cmake_minimum_required(VERSION 3.16)
project(mini_trantor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(mini_trantor
    mini/net/InetAddress.cc
    mini/net/Buffer.cc
    mini/net/SocketsOps.cc
    mini/net/Socket.cc
    mini/net/Channel.cc
    mini/net/Poller.cc
    mini/net/EPollPoller.cc
    mini/net/EventLoop.cc
    mini/net/Acceptor.cc
    mini/net/EventLoopThread.cc
    mini/net/EventLoopThreadPool.cc
    mini/net/TcpConnection.cc
    mini/net/TcpServer.cc
)

target_include_directories(mini_trantor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mini_trantor PUBLIC cxx_std_20)
target_link_libraries(mini_trantor PUBLIC pthread)

add_executable(echo_server examples/echo_server/main.cpp)
target_link_libraries(echo_server PRIVATE mini_trantor)

add_executable(coroutine_echo_server examples/coroutine_echo_server/main.cpp)
target_link_libraries(coroutine_echo_server PRIVATE mini_trantor)

add_executable(test_buffer tests/test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE mini_trantor)
add_test(NAME test_buffer COMMAND test_buffer)

add_executable(test_coroutine tests/test_coroutine.cpp)
target_link_libraries(test_coroutine PRIVATE mini_trantor)
add_test(NAME test_coroutine COMMAND test_coroutine)

add_executable(test_event_loop tests/test_event_loop.cpp)
target_link_libraries(test_event_loop PRIVATE mini_trantor)
add_test(NAME test_event_loop COMMAND test_event_loop)

add_executable(test_channel tests/test_channel.cpp)
target_link_libraries(test_channel PRIVATE mini_trantor)
add_test(NAME test_channel COMMAND test_channel)

add_executable(test_poller_contract tests/test_poller_contract.cpp)
target_link_libraries(test_poller_contract PRIVATE mini_trantor)
add_test(NAME test_poller_contract COMMAND test_poller_contract)

add_executable(test_tcp_connection tests/test_tcp_connection.cpp)
target_link_libraries(test_tcp_connection PRIVATE mini_trantor)
add_test(NAME test_tcp_connection COMMAND test_tcp_connection)

add_executable(test_tcp_server tests/test_tcp_server.cpp)
target_link_libraries(test_tcp_server PRIVATE mini_trantor)
add_test(NAME test_tcp_server COMMAND test_tcp_server)

add_executable(test_event_loop_thread_pool tests/test_event_loop_thread_pool.cpp)
target_link_libraries(test_event_loop_thread_pool PRIVATE mini_trantor)
add_test(NAME test_event_loop_thread_pool COMMAND test_event_loop_thread_pool)
