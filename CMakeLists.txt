cmake_minimum_required(VERSION 3.16)
project(mini_trantor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(mini_trantor
    mini/net/InetAddress.cc
    mini/net/Buffer.cc
    mini/net/SocketsOps.cc
    mini/net/Socket.cc
    mini/net/Channel.cc
    mini/net/Poller.cc
    mini/net/EPollPoller.cc
    mini/net/EventLoop.cc
    mini/net/TimerQueue.cc
    mini/net/Acceptor.cc
    mini/net/Connector.cc
    mini/net/EventLoopThread.cc
    mini/net/EventLoopThreadPool.cc
    mini/net/TcpConnection.cc
    mini/net/TcpClient.cc
    mini/net/TcpServer.cc
    mini/net/TlsContext.cc
    mini/net/DnsResolver.cc
    mini/http/HttpRequest.cc
    mini/http/HttpResponse.cc
    mini/http/HttpContext.cc
    mini/http/HttpServer.cc
    mini/ws/WebSocketCodec.cc
    mini/ws/WebSocketHandshake.cc
    mini/ws/WebSocketConnection.cc
    mini/ws/WebSocketServer.cc
    mini/rpc/RpcCodec.cc
    mini/rpc/RpcChannel.cc
    mini/rpc/RpcServer.cc
    mini/rpc/RpcClient.cc
)

add_library(mini_trantor::mini_trantor ALIAS mini_trantor)

target_include_directories(
    mini_trantor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(mini_trantor PUBLIC cxx_std_20)
target_link_libraries(mini_trantor PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_executable(echo_server examples/echo_server/main.cpp)
target_link_libraries(echo_server PRIVATE mini_trantor)

add_executable(coroutine_echo_server examples/coroutine_echo_server/main.cpp)
target_link_libraries(coroutine_echo_server PRIVATE mini_trantor)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(
    TARGETS mini_trantor
    EXPORT mini_trantorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/mini/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mini
    FILES_MATCHING
        PATTERN "*.h"
)

set(mini_trantor_config_install_dir ${CMAKE_INSTALL_LIBDIR}/cmake/mini_trantor)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mini_trantorConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mini_trantorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mini_trantorConfig.cmake
    INSTALL_DESTINATION ${mini_trantor_config_install_dir}
)

install(
    EXPORT mini_trantorTargets
    FILE mini_trantorTargets.cmake
    NAMESPACE mini_trantor::
    DESTINATION ${mini_trantor_config_install_dir}
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/mini_trantorConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/mini_trantorConfigVersion.cmake
    DESTINATION ${mini_trantor_config_install_dir}
)
