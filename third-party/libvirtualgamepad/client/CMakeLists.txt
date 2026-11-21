cmake_minimum_required(VERSION 3.21)

add_library(libvirtualgamepad_client STATIC client.cpp)
add_library(libvirtualgamepad::client ALIAS libvirtualgamepad_client)

target_compile_features(libvirtualgamepad_client PUBLIC cxx_std_20)
target_include_directories(libvirtualgamepad_client PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(libvirtualgamepad_client PUBLIC setupapi)
