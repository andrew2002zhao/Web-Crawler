cmake_minimum_required(VERSION 3.13)
project(ECE252-Project-5)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(LibXml2 REQUIRED)

add_executable(multithreaded_crawler multithreaded_crawler.c)

include_directories(lib/includes)

add_library(queue STATIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/src/queue.c)

target_link_libraries(multithreaded_crawler PUBLIC queue  LibXml2::LibXml2 -lcurl)

set_property(TARGET queue multithreaded_crawler PROPERTY C_STANDARD 11)

install(
    TARGETS
    multithreaded_crawler
    DESTINATION
    bin

)
