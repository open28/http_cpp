cmake_minimum_required(VERSION 3.18)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TARGET reactor_c)
project(${TARGET})

find_package(spdlog REQUIRED)


add_executable(${TARGET} main.cpp Server.cpp)
target_link_libraries(${TARGET} spdlog::spdlog)