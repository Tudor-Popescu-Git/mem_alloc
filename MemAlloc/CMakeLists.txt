# MemAlloc sub-project: the allocator plus its test harness.

add_executable (MemAlloc
    "mem_op.c"
    "test_mem_alloc.c")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET MemAlloc PROPERTY C_STANDARD 11)
endif()

if(MSVC)
    # AddressSanitizer in Debug builds.
    target_compile_options(MemAlloc PRIVATE $<$<CONFIG:Debug>:/fsanitize=address>)

    # VS doesn't put the ASan runtime on PATH for CMake projects, so copy the
    # DLL next to the exe. It sits beside cl.exe in the MSVC toolchain.
    get_filename_component(_cl_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    add_custom_command(TARGET MemAlloc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_cl_dir}/clang_rt.asan_dynamic-x86_64.dll"
            "$<TARGET_FILE_DIR:MemAlloc>")
else()
    target_compile_options(MemAlloc PRIVATE $<$<CONFIG:Debug>:-fsanitize=address -g>)
    target_link_options(MemAlloc    PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
endif()