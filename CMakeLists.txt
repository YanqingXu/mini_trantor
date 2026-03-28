cmake_minimum_required(VERSION 3.10)
project(mini_trantor)

set(CMAKE_CXX_STANDARD 20)

include_directories(include)

add_executable(server
    src/main.cpp
    src/EventLoop.cpp
    src/Channel.cpp
    src/Poller.cpp
)
