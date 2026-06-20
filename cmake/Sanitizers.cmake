# Sanitizers.cmake — Add sanitizer flags to guarded builds.
#
# Usage:
#   include(Sanitizers)
#   enable_sanitizers(mini_trantor)
#
# Sanitizers are only enabled in Debug mode unless fuzzing is enabled.
# In Release mode, no flags are added (zero runtime overhead).

function(enable_sanitizers target)
    if(NOT MINI_ENABLE_ASAN_UBSAN AND NOT MINI_ENABLE_TSAN AND NOT MINI_ENABLE_FUZZ)
        return()
    endif()

    if(MINI_ENABLE_ASAN_UBSAN AND MINI_ENABLE_TSAN)
        message(FATAL_ERROR "MINI_ENABLE_ASAN_UBSAN and MINI_ENABLE_TSAN cannot be enabled together.")
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GCC.")
    endif()

    set(sanitizer_compile_options)
    set(sanitizer_link_options)

    if(MINI_ENABLE_TSAN)
        list(APPEND sanitizer_compile_options -fsanitize=thread -fno-omit-frame-pointer)
        list(APPEND sanitizer_link_options -fsanitize=thread)
    else()
        list(APPEND sanitizer_compile_options -fsanitize=address,undefined -fno-omit-frame-pointer)
        list(APPEND sanitizer_link_options -fsanitize=address,undefined)
    endif()

    if(MINI_ENABLE_FUZZ)
        target_compile_options(${target} PUBLIC ${sanitizer_compile_options})
        target_link_options(${target} PUBLIC ${sanitizer_link_options})
        return()
    endif()

    foreach(flag IN LISTS sanitizer_compile_options)
        target_compile_options(${target} PUBLIC "$<$<CONFIG:Debug>:${flag}>")
    endforeach()

    foreach(flag IN LISTS sanitizer_link_options)
        target_link_options(${target} PUBLIC "$<$<CONFIG:Debug>:${flag}>")
    endforeach()
endfunction()
