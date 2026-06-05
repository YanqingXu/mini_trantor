#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "mini_trantor::mini_trantor" for configuration "Debug"
set_property(TARGET mini_trantor::mini_trantor APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(mini_trantor::mini_trantor PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libmini_trantor.a"
  )

list(APPEND _cmake_import_check_targets mini_trantor::mini_trantor )
list(APPEND _cmake_import_check_files_for_mini_trantor::mini_trantor "${_IMPORT_PREFIX}/lib/libmini_trantor.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
