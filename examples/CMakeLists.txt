add_executable(fl_peer tcp_peer/fl_peer.c)
target_link_libraries(fl_peer PRIVATE framelink)
set_target_properties(fl_peer PROPERTIES C_STANDARD 99)
if(WIN32)
    target_link_libraries(fl_peer PRIVATE ws2_32)
else()
    # clock_gettime and getaddrinfo are POSIX, not plain C99.
    target_compile_definitions(fl_peer PRIVATE _POSIX_C_SOURCE=200809L)
endif()
