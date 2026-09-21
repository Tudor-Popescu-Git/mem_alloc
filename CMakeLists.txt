# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required (VERSION 3.10)

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