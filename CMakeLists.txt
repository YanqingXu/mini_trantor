cmake_minimum_required(VERSION 3.16)
project(mini_trantor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)

option(MINI_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer for Debug guarded builds" OFF)
option(MINI_ENABLE_TSAN "Enable ThreadSanitizer for Debug guarded builds" OFF)
option(MINI_ENABLE_FUZZ "Build libFuzzer protocol parser targets" OFF)
if(WIN32)
    option(MINI_ENABLE_TLS "Build OpenSSL/TLS integration" OFF)
else()
    option(MINI_ENABLE_TLS "Build OpenSSL/TLS integration" ON)
endif()

if(MINI_ENABLE_TSAN AND MINI_ENABLE_FUZZ)
    message(FATAL_ERROR "MINI_ENABLE_TSAN cannot be combined with MINI_ENABLE_FUZZ.")
endif()

if(MINI_ENABLE_FUZZ AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MINI_ENABLE_FUZZ requires Clang/clang++ for libFuzzer support.")
endif()

find_package(Threads REQUIRED)
if(MINI_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
endif()

set(MINI_TRANTOR_SOURCES
    mini/base/Logger.cc
    mini/base/MetricsExporter.cc
    mini/net/InetAddress.cc
    mini/net/Buffer.cc
    mini/net/Socket.cc
    mini/net/Channel.cc
    mini/net/Poller.cc
    mini/net/poller/PollerFactory.cc
    mini/net/EventLoop.cc
    mini/net/TimerQueue.cc
    mini/net/Acceptor.cc
    mini/net/Connector.cc
    mini/net/EventLoopThread.cc
    mini/net/EventLoopThreadPool.cc
    mini/net/broadcast/BroadcastRouter.cc
    mini/net/broadcast/BroadcastDispatcher.cc
    mini/net/TcpConnection.cc
    mini/net/detail/ConnectionCallbackDispatcher.cc
    mini/net/detail/ConnectionAwaiterRegistry.cc
    mini/net/detail/ConnectionBackpressureController.cc
    mini/net/detail/ConnectionTransport.cc
    mini/net/buffer/Payload.cc
    mini/net/buffer/PayloadPool.cc
    mini/net/TcpClient.cc
    mini/net/TcpServer.cc
    mini/net/TlsContext.cc
    mini/net/udp/IcmpPathMtuListener.cc
    mini/net/udp/PathMtuSignalAdapter.cc
    mini/net/udp/UdpSocket.cc
    mini/net/udp/UdpServer.cc
    mini/net/transport/PathMtuCache.cc
    mini/net/framing/PacketFramer.cc
    mini/codec/ProtobufAdapter.cc
    mini/codec/FlatBuffersAdapter.cc
    mini/net/kcp/KcpCodec.cc
    mini/net/kcp/KcpTransport.cc
    mini/net/kcp/KcpSession.cc
    mini/game/PlayerSession.cc
    mini/game/SessionManager.cc
    mini/game/GameServerPipeline.cc
    mini/game/logic/GameCommandQueue.cc
    mini/game/logic/LogicLoop.cc
    mini/net/DnsResolver.cc
    mini/net/SignalWatcher.cc
    mini/net/ProtocolConnectionAdapter.cc
    mini/http/HttpRequest.cc
    mini/http/HttpResponse.cc
    mini/http/HttpResponseContext.cc
    mini/http/HttpContext.cc
    mini/http/HttpClient.cc
    mini/http/HttpServer.cc
    mini/ws/WebSocketCodec.cc
    mini/ws/WebSocketHandshake.cc
    mini/ws/WebSocketConnection.cc
    mini/ws/WebSocketServer.cc
    mini/rpc/RpcCodec.cc
    mini/rpc/RpcChannel.cc
    mini/rpc/RpcServer.cc
    mini/rpc/RpcClient.cc
    mini/rpc/RpcConnectionPool.cc
)

if(WIN32)
    list(APPEND MINI_TRANTOR_SOURCES
        mini/net/platform/SocketsOps_win.cc
        mini/net/platform/Wakeup_win.cc
        mini/net/poller/SelectPoller.cc
    )
else()
    list(APPEND MINI_TRANTOR_SOURCES
        mini/net/platform/SocketsOps_linux.cc
        mini/net/platform/Wakeup_linux.cc
        mini/net/poller/EPollPoller.cc
    )
endif()

add_library(mini_trantor ${MINI_TRANTOR_SOURCES})

add_library(mini_trantor::mini_trantor ALIAS mini_trantor)

target_include_directories(
    mini_trantor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(mini_trantor PUBLIC cxx_std_23)
target_link_libraries(mini_trantor PUBLIC Threads::Threads)
target_compile_definitions(mini_trantor PUBLIC MINI_ENABLE_TLS=$<BOOL:${MINI_ENABLE_TLS}>)
if(MSVC)
    target_compile_options(mini_trantor PUBLIC /utf-8)
endif()
if(MINI_ENABLE_TLS)
    target_link_libraries(mini_trantor PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(WIN32)
    target_compile_definitions(mini_trantor PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(mini_trantor PUBLIC ws2_32)
endif()
enable_sanitizers(mini_trantor)

add_executable(echo_server examples/echo_server/main.cpp)
target_link_libraries(echo_server PRIVATE mini_trantor)

add_executable(coroutine_echo_server examples/coroutine_echo_server/main.cpp)
target_link_libraries(coroutine_echo_server PRIVATE mini_trantor)

add_executable(game_server examples/game_server/main.cpp)
target_link_libraries(game_server PRIVATE mini_trantor)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(MINI_ENABLE_FUZZ)
    add_subdirectory(tests/fuzz)
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
