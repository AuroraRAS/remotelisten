cmake_minimum_required(VERSION 3.5)

project(remotelisten)


add_executable(remotelisten main.c)
set_property(TARGET remotelisten PROPERTY C_STANDARD 11)
target_link_libraries(remotelisten uv)
