function(add_mini_fuzz_target target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE mini_trantor)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_FLAGS MATCHES "-stdlib=libc[+][+]")
        # Ubuntu's prebuilt libFuzzer uses the libstdc++ ABI even when the
        # instrumented application uses libc++. This is a harness-only dependency.
        # Pass directly to ld; clang rewrites a driver-level -lstdc++ to libc++.
        target_link_libraries(${target} PRIVATE "-Wl,-lstdc++")
    endif()
endfunction()

add_mini_fuzz_target(fuzz_packet_framer framing/fuzz_packet_framer.cpp)

# Mutations go to the build tree; checked-in seeds cover complete/partial/oversized frames.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/framing/corpus/"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/framing-corpus")
