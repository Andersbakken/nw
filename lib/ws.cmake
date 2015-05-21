cmake_minimum_required(VERSION 2.8)
include_directories(${CMAKE_CURRENT_LIST_DIR} ${WSLAY_INCLUDE})
add_library(nw ${NW_LIBRARY_MODE} ${CMAKE_CURRENT_LIST_DIR}/NW.cpp)
target_link_libraries(nw ${WSLAY_LIBS})
