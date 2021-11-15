cmake_minimum_required(VERSION 3.0.0)
project(bouncer VERSION 0.1.0)

include(CTest)
enable_testing()

add_executable(bouncer bouncer.c)
install(TARGETS bouncer DESTINATION bin)
