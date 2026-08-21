cmake_minimum_required(VERSION 3.16)
project(pcap_ssd_tool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# libpcap has no CMake config file on most systems — find it manually.
find_path(PCAP_INCLUDE_DIR NAMES pcap.h)
find_library(PCAP_LIBRARY NAMES pcap)

if(NOT PCAP_INCLUDE_DIR OR NOT PCAP_LIBRARY)
    message(FATAL_ERROR "libpcap not found. Install libpcap-dev (Debian/Ubuntu) or libpcap-devel (Fedora/RHEL).")
endif()

add_library(pcap_ssd_core
    src/capture_reader.cpp
    src/ssd_writer_pool.cpp
)
target_include_directories(pcap_ssd_core PUBLIC include ${PCAP_INCLUDE_DIR})
target_link_libraries(pcap_ssd_core PUBLIC ${PCAP_LIBRARY} OpenSSL::Crypto Threads::Threads)

add_executable(pcap_ssd_tool src/main.cpp)
target_link_libraries(pcap_ssd_tool PRIVATE pcap_ssd_core)

add_executable(bench_ssd_writer bench/bench_ssd_writer.cpp)
target_link_libraries(bench_ssd_writer PRIVATE pcap_ssd_core)

enable_testing()
# add_subdirectory(tests)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()