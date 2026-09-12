# Build/install contract: consumers need only the selected dependencies and APIs.
set(prefix "${PROJECT_BINARY_DIR}/_install_contract")
set(consumer_build "${PROJECT_BINARY_DIR}/_consumer_contract")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}"
    --config "${CONFIG}" --prefix "${prefix}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Library installation failed: ${result}")
endif()
foreach(retired IN ITEMS game http ws rpc codec net/kcp net/udp net/transport
        net/broadcast net/buffer base/MetricsExporter.h net/ProtocolConnection.h)
    file(GLOB_RECURSE retired_headers "${prefix}/include/mini/${retired}/*.h")
    if(retired_headers OR (EXISTS "${prefix}/include/mini/${retired}" AND
            NOT IS_DIRECTORY "${prefix}/include/mini/${retired}"))
        message(FATAL_ERROR "Retired API leaked into install: ${retired}")
    endif()
endforeach()
set(options)
if(NOT TLS_ENABLED)
    list(APPEND options -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${PROJECT_SOURCE_DIR}/tests/package" -B "${consumer_build}"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
    "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_BUILD_TYPE=${CONFIG}" ${options}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed package configuration failed: ${result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${CONFIG}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed package compilation/link failed: ${result}")
endif()
# Compile and link are sufficient for sanitizer packages; runtime is covered by CTest.
if(NOT SANITIZED AND NOT TSAN_ENABLED)
    set(consumer "${consumer_build}/consumer")
    if(WIN32)
        set(consumer "${consumer_build}/${CONFIG}/consumer.exe")
    endif()
    execute_process(COMMAND "${consumer}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installed consumer failed: ${result}")
    endif()
endif()
