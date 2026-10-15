# Unity is third-party code, so it is built without the library's -Werror.
add_library(unity STATIC vendor/unity/unity.c)
target_include_directories(unity PUBLIC vendor/unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

add_executable(test_codec test_codec.c)
target_include_directories(test_codec PRIVATE vectors)
target_link_libraries(test_codec PRIVATE framelink unity)
set_target_properties(test_codec PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(test_codec PRIVATE -Wall -Wextra)
endif()

add_test(NAME codec COMMAND test_codec)
