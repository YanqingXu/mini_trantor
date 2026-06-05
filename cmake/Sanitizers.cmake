# Sanitizers.cmake — Add sanitizer flags to Debug builds.
#
# Usage:
#   include(Sanitizers)
#   enable_sanitizers(mini_trantor)
#
# Sanitizers are only enabled in Debug mode.
# In Release mode, no flags are added (zero runtime overhead).

function(enable_sanitizers target)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")

    target_compile_options(${target} PRIVATE ${SANITIZER_FLAGS})
    target_link_options(${target} PRIVATE ${SANITIZER_FLAGS})
endfunction()
