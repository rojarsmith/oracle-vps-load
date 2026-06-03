cmake_minimum_required(VERSION 3.16)

project(mock_load VERSION 1.0.0 LANGUAGES CXX)

add_executable(mock-load mock-load.cpp)

target_compile_features(mock-load PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(mock-load PRIVATE /W4 /permissive-)
else()
    target_compile_options(mock-load PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_compile_definitions(mock-load PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(mock-load PRIVATE Iphlpapi Ws2_32)
endif()

if(MINGW)
    target_link_options(mock-load PRIVATE -static -static-libgcc -static-libstdc++)
endif()
