cmake_minimum_required(VERSION 3.16)
project(mini_trantor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

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

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
