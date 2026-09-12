cmake_minimum_required(VERSION 3.25)
project(mini_trantor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <expected>
#include <thread>
static_assert(sizeof(std::jthread) > 0);
int main() { std::expected<int, int> result(1); return result.value() - 1; }"
    MINI_HAS_CXX23_RUNTIME)
if(NOT MINI_HAS_CXX23_RUNTIME)
    message(FATAL_ERROR "std::expected and std::jthread are required. Use a compatible compiler/standard library pair; see README for the tested Clang/libc++ flags.")
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)

option(MINI_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer for Debug guarded builds" OFF)
option(MINI_ENABLE_TSAN "Enable ThreadSanitizer for Debug guarded builds" OFF)
option(MINI_ENABLE_FUZZ "Build libFuzzer protocol parser targets" OFF)
option(MINI_ENABLE_TLS "Build optional OpenSSL/TLS integration" OFF)
option(MINI_BUILD_EXAMPLES "Build TCP echo examples" ON)

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
    mini/net/InetAddress.cc
    mini/net/Buffer.cc
    mini/net/Socket.cc
    mini/net/Channel.cc
    mini/net/Poller.cc
    mini/net/poller/PollerFactory.cc
    mini/net/EventLoop.cc
    mini/net/LoopHandle.cc
    mini/net/TimerQueue.cc
    mini/net/Acceptor.cc
    mini/net/Connector.cc
    mini/net/EventLoopThread.cc
    mini/net/EventLoopThreadPool.cc
    mini/net/TcpConnection.cc
    mini/net/detail/ConnectionCallbackDispatcher.cc
    mini/net/detail/ConnectionAwaiterRegistry.cc
    mini/net/detail/ConnectionBackpressureController.cc
    mini/net/detail/ConnectionTransport.cc
    mini/net/TcpClient.cc
    mini/net/TcpServer.cc
    mini/net/TlsContext.cc
    mini/net/framing/PacketFramer.cc
    mini/net/DnsResolver.cc
    mini/net/SignalWatcher.cc
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

if(MINI_BUILD_EXAMPLES)
add_executable(echo_server examples/echo_server/main.cpp)
target_link_libraries(echo_server PRIVATE mini_trantor)

add_executable(coroutine_echo_server examples/coroutine_echo_server/main.cpp)
target_link_libraries(coroutine_echo_server PRIVATE mini_trantor)

endif()

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

# Install only maintained header directories, including no empty retired directories.
file(GLOB MINI_PUBLIC_HEADERS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    mini/base/*.h mini/coroutine/*.h mini/net/*.h mini/net/detail/*.h
    mini/net/platform/*.h mini/net/poller/*.h mini/net/framing/*.h)
foreach(header IN LISTS MINI_PUBLIC_HEADERS)
    get_filename_component(header_dir "${header}" DIRECTORY)
    install(FILES "${header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${header_dir}")
endforeach()

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
