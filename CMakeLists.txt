cmake_minimum_required(VERSION 3.15)
project(bedrockkit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Link the MSVC runtime statically so the .exe runs on any Windows PC
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(bedrockkit src/main.cpp)
target_include_directories(bedrockkit PRIVATE include)
if(WIN32)
  target_link_libraries(bedrockkit PRIVATE ws2_32)
endif()
if(NOT MSVC)
  target_compile_options(bedrockkit PRIVATE -O2 -Wall -Wextra)
endif()
