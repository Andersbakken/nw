cmake_minimum_required(VERSION 2.8)
include_directories(${CMAKE_CURRENT_LIST_DIR})
add_library(nw ${NW_LIBRARY_MODE} ${CMAKE_CURRENT_LIST_DIR}/NW.cpp)
