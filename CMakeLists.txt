cmake_minimum_required(VERSION 3.25)

project(
    "Template"
    DESCRIPTION ""
    VERSION 0.0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS NO)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if (PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(template_BUILD_EXAMPLES "Build Template examples" OFF)

if(template_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

add_library(template INTERFACE)

target_include_directories(template INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# add_subdirectory(external/xxx SYSTEM)
# target_link_libraries(
#     ${target_name}
#     PRIVATE xxx
# )

# Debugging options

option(template_ENABLE_SANITIZERS "Enable ASan/UBSan" OFF)

if (template_ENABLE_SANITIZERS)
    target_compile_options(
        template
        PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(
        template
        PRIVATE
        -fsanitize=address,undefined
    )
endif()
