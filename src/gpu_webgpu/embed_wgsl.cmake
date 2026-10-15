# Writes a WGSL file as a C++ header: inline constexpr char kWgsl_<NAME>[].
# cmake -DIN=<file.wgsl> -DOUT=<file.h> -DNAME=<identifier> -P embed_wgsl.cmake
file(READ "${IN}" _wgsl)
file(WRITE "${OUT}"
  "// Generated from ${IN} by embed_wgsl.cmake. Do not edit.\n"
  "#pragma once\n"
  "inline constexpr char kWgsl_${NAME}[] = R\"wgsl(${_wgsl})wgsl\";\n")
