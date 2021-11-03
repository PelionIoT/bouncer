cmake_minimum_required(VERSION 3.0.0)
project(socket-proxy VERSION 0.1.0)

include(CTest)
enable_testing()

add_executable(socket-proxy socket-proxy.c)
install(TARGETS socket-proxy DESTINATION bin)
