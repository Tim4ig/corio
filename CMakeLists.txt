cmake_minimum_required(VERSION 4.2)
project(corio VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED on)

set(SRC_FILES
  src/main.cc
)

add_executable(${PROJECT_NAME} ${SRC_FILES})
