# Each harness is built two ways:
#   fuzz_<name>          libFuzzer binary, only with FL_FUZZ=ON and clang
#   fuzz_<name>_replay   plain binary that replays the committed corpus. It
#                        builds with any compiler and ctest runs it as a
#                        regression test.
set(FL_FUZZ_HARNESSES decoder roundtrip)

foreach(name IN LISTS FL_FUZZ_HARNESSES)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_${name}.c)

    if(FL_FUZZ)
        add_executable(fuzz_${name} ${src})
        target_link_libraries(fuzz_${name} PRIVATE framelink)
        target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    endif()

    if(FL_BUILD_TESTS)
        add_executable(fuzz_${name}_replay ${src} ${CMAKE_CURRENT_SOURCE_DIR}/standalone_main.c)
        target_link_libraries(fuzz_${name}_replay PRIVATE framelink)
        set_target_properties(fuzz_${name}_replay PROPERTIES C_STANDARD 99)
        file(GLOB corpus CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}/*)
        list(LENGTH corpus corpus_size)
        if(corpus_size GREATER 0)
            add_test(NAME replay_${name} COMMAND fuzz_${name}_replay ${corpus})
        endif()
    endif()
endforeach()
