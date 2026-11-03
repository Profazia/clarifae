cmake_minimum_required(VERSION 3.18)
project(clarifae_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(ANDROID)
  message(FATAL_ERROR "This suite uses explicit host-only Android API stubs")
endif()
option(CLARIFAE_SANITIZERS "Enable host AddressSanitizer and UBSan" ON)
if(CLARIFAE_SANITIZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Sanitizer tests require Clang or GCC")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()
set(RNNOISE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../native/third_party/rnnoise"
    CACHE PATH "External RNNoise v0.2 release source including trained tables")
if(NOT EXISTS "${RNNOISE_DIR}/include/rnnoise.h" OR
   NOT EXISTS "${RNNOISE_DIR}/src/rnnoise_data.c")
  message(FATAL_ERROR "Set RNNOISE_DIR to a prepared RNNoise v0.2 release tree; tests do not download dependencies")
endif()
set(NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../native")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/host-stubs"
                    "${NATIVE}" "${NATIVE}/include")
file(GLOB RNN_SRCS "${RNNOISE_DIR}/src/*.c")
list(FILTER RNN_SRCS EXCLUDE REGEX "(dump_features|dump_rnnoise_tables|write_weights)")
add_library(rnnoise_test STATIC ${RNN_SRCS})
set_target_properties(rnnoise_test PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(rnnoise_test PRIVATE RNNOISE_BUILD)
target_include_directories(rnnoise_test PUBLIC "${RNNOISE_DIR}/include"
                                               "${RNNOISE_DIR}/src")

add_library(clarifae_test SHARED "${NATIVE}/effect.c" "${NATIVE}/config.c"
    "${NATIVE}/backend_rnnoise.c" "${NATIVE}/resampler.c" "${NATIVE}/ringbuf.c"
    host-stubs/system_properties.c host-stubs/backend_optional.c)
target_link_libraries(clarifae_test PRIVATE rnnoise_test m)
add_executable(effect_smoke effect_smoke.c)
target_link_libraries(effect_smoke PRIVATE m ${CMAKE_DL_LIBS})
add_executable(backend_pipeline backend_pipeline.c "${NATIVE}/backend_rnnoise.c"
    "${NATIVE}/resampler.c" "${NATIVE}/ringbuf.c" host-stubs/backend_optional.c)
target_link_libraries(backend_pipeline PRIVATE rnnoise_test m)
add_executable(config_watch config_watch.c "${NATIVE}/config.c"
    host-stubs/system_properties.c)
add_executable(effect_contract effect_contract.c "${NATIVE}/effect.c"
    "${NATIVE}/config.c" host-stubs/system_properties.c host-stubs/backend_probe.c)
target_link_libraries(effect_contract PRIVATE m)
add_executable(output_matrix output_matrix.c "${NATIVE}/effect.c"
    "${NATIVE}/config.c" host-stubs/system_properties.c host-stubs/backend_probe.c)
target_link_libraries(output_matrix PRIVATE m)
foreach(target clarifae_test effect_smoke backend_pipeline config_watch effect_contract output_matrix)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endforeach()

enable_testing()
add_test(NAME effect_smoke COMMAND effect_smoke $<TARGET_FILE:clarifae_test>)
add_test(NAME backend_pipeline COMMAND backend_pipeline)
add_test(NAME config_watch COMMAND config_watch)
add_test(NAME effect_contract COMMAND effect_contract)
add_test(NAME output_matrix COMMAND output_matrix)
set_tests_properties(effect_smoke backend_pipeline config_watch effect_contract output_matrix
                     PROPERTIES TIMEOUT 180)
if(APPLE AND CLARIFAE_SANITIZERS)
  # Apple ASan does not supply LeakSanitizer.
  set_tests_properties(effect_smoke backend_pipeline config_watch effect_contract output_matrix
                       PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endif()
