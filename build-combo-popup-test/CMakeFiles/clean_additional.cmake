# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles\\md4c_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\md4c_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\mdv_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\mdv_autogen.dir\\ParseCache.txt"
  "md4c_autogen"
  "mdv_autogen"
  )
endif()
