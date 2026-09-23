cmake_minimum_required(VERSION 3.20)
project(sncf_downstream_consumer LANGUAGES CXX)

# The consumer deliberately relies on find_package only: no repository sources,
# no repository include paths, no repository warning flags.
find_package(sncf 1.0 REQUIRED CONFIG)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE sncf::sncf)
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# The consumer builds with its own warning policy, proving the repository's
# strict flags do not leak through the exported target.
if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Werror)
endif()
