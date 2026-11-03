# Usage: include(.../ClarifaeDTLN.cmake); clarifae_enable_dtln(your_target)
# Parent already owns/link-compiles engine/resampler.c. Do not add it twice.
get_filename_component(_CLARIFAE_DTLN_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT TARGET Clarifae::TfLite)
 add_library(Clarifae::TfLite SHARED IMPORTED GLOBAL)
 set_target_properties(Clarifae::TfLite PROPERTIES
  IMPORTED_LOCATION "${_CLARIFAE_DTLN_DIR}/runtime/android-arm64/libtensorflowlite_jni.so"
  IMPORTED_SONAME "libtensorflowlite_jni.so"
  INTERFACE_INCLUDE_DIRECTORIES "${_CLARIFAE_DTLN_DIR}/runtime/include;${_CLARIFAE_DTLN_DIR}/runtime/include/external/org_tensorflow")
endif()
function(clarifae_enable_dtln target)
 target_sources(${target} PRIVATE "${_CLARIFAE_DTLN_DIR}/../../engine/backend_dtln.c")
 target_compile_definitions(${target} PRIVATE CLARIFAE_WITH_DTLN=1)
 target_link_libraries(${target} PRIVATE Clarifae::TfLite m)
endfunction()
