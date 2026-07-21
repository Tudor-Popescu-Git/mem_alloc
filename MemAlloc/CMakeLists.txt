# CMakeList.txt : CMake project for MemAlloc, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (MemAlloc "main.cpp" "MemAlloc.h" "mem_op.h" "mem_op.c" "mem_something.h" "mem_op_natvis.h")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET MemAlloc PROPERTY CXX_STANDARD 20)
endif()

# TODO: Add tests and install targets if needed.
