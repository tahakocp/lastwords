cmake_minimum_required(VERSION 3.13)
project(lastwords C)

# Ring size and options go through the target, e.g.
#   target_compile_definitions(lastwords PUBLIC LW_RING_SIZE=4096)
add_library(lastwords STATIC src/lastwords.c)
target_include_directories(lastwords PUBLIC src)
set_target_properties(lastwords PROPERTIES C_STANDARD 99)
