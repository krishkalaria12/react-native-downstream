cmake_minimum_required(VERSION 3.19)
project(downstream_code LANGUAGES C CXX)
get_filename_component(CODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
# Resolve the peer from this package so pnpm and published node_modules layouts
# use the same headers rather than depending on a sibling checkout.
execute_process(
  COMMAND node -p "require.resolve('@react-native-downstream/core/package.json', {paths: [process.argv[1]]})" ${CODE_DIR}
  OUTPUT_VARIABLE CORE_PACKAGE_JSON OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
get_filename_component(CORE_DIR ${CORE_PACKAGE_JSON} DIRECTORY)
add_library(downstream_code SHARED
  ${CODE_DIR}/cpp/CodeHighlighterPlugin.cpp
  ${CODE_DIR}/cpp/NativeLexer.cpp
  ${CODE_DIR}/cpp/IdentifierClasses.cpp
  ${CODE_DIR}/cpp/HighlightsModule.c)
target_include_directories(downstream_code PRIVATE
  ${CORE_DIR}/src/cpp/highlight
  ${CODE_DIR}/cpp
  ${CODE_DIR}/cpp/wasm
  ${CODE_DIR}/cpp/wasm/wasm-rt
  ${CODE_DIR}/cpp/wasm/simde)
set_target_properties(downstream_code PROPERTIES
  C_STANDARD 11 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
