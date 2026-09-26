# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required (VERSION 3.10)

# On Linux/WSL builds, prefer Clang when it is installed. This must come
# before project(), because that is when CMake locks in the compiler.
# Falls back to the default compiler (GCC) when Clang is not present, and
# is ignored entirely on Windows/MSVC builds.
if (UNIX AND NOT DEFINED CMAKE_C_COMPILER)
    find_program(MEM_CLANG_C   clang)
    find_program(MEM_CLANG_CXX clang++)
    if (MEM_CLANG_C AND MEM_CLANG_CXX)
        set(CMAKE_C_COMPILER   "${MEM_CLANG_C}"   CACHE FILEPATH "C compiler")
        set(CMAKE_CXX_COMPILER "${MEM_CLANG_CXX}" CACHE FILEPATH "C++ compiler")
    endif()
endif()

# Debug info format: ProgramDatabase (/Zi), not EditAndContinue (/ZI),
# because Hot Reload's /ZI is incompatible with AddressSanitizer.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>")
endif()

project ("MemAlloc")

# /RTC1 (from CMake's default debug flags) is incompatible with ASan.
if(MSVC)
    string(REGEX REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# Include sub-projects.
add_subdirectory ("MemAlloc")