cmake_minimum_required(VERSION 3.13)
set(CMAKE_VERBOSE_MAKEFILE on)

set(LIB_LITERAL DownstreamSpec)
set(LIB_TARGET_NAME react_codegen_${LIB_LITERAL})

set(LIB_ANDROID_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(LIB_CPP_DIR ${LIB_ANDROID_DIR}/../cpp)
set(LIB_GENERATED_JNI_DIR ${LIB_ANDROID_DIR}/build/generated/source/codegen/jni)
set(LIB_GENERATED_COMPONENTS_DIR ${LIB_GENERATED_JNI_DIR}/react/renderer/components/${LIB_LITERAL})

file(GLOB LIB_CODEGEN_SRCS CONFIGURE_DEPENDS
  ${LIB_GENERATED_JNI_DIR}/*.cpp
  ${LIB_GENERATED_COMPONENTS_DIR}/*.cpp)
file(GLOB LIB_ENGINE_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/downstream/*.cpp
  ${LIB_CPP_DIR}/session/*.cpp
  ${LIB_CPP_DIR}/fabric/*.cpp
  ${LIB_CPP_DIR}/remend/*.cpp)
# Engine-independent highlighting policy. The optional lexer is a separate package.
file(GLOB LIB_HIGHLIGHT_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/highlight/*.c
  ${LIB_CPP_DIR}/highlight/*.cpp)
file(GLOB LIB_THEME_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/theme/*.cpp)
# Math is its own directory rather than a subdirectory of `downstream`, so it needs
# its own glob. Omitting it does not fail the configure step: the library links and
# then fails on the first undefined `downstream::math*` symbol, which reads like a
# platform problem rather than a missing source.
file(GLOB LIB_MATH_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/math/*.cpp)
file(GLOB LIB_JNI_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# Other libraries, such as Enriched Markdown, link their own md4c. Prefixed,
# hidden symbols keep the linker from binding our calls to theirs.
set(MD4C_SYMBOL_PREFIXES md_parse=downstream_md_parse entity_lookup=downstream_entity_lookup)

add_library(downstream_md4c STATIC
  ${LIB_CPP_DIR}/md4c/md4c.c
  ${LIB_CPP_DIR}/md4c/entity.c)
target_include_directories(downstream_md4c PUBLIC ${LIB_CPP_DIR}/md4c)
target_compile_definitions(downstream_md4c PRIVATE MD4C_USE_UTF8=1 ${MD4C_SYMBOL_PREFIXES})
set_target_properties(downstream_md4c PROPERTIES
  C_STANDARD 11
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

# The core retains incremental token policy and the optional-lexer adapter.
add_library(downstream_highlight STATIC ${LIB_HIGHLIGHT_SRCS})
target_include_directories(downstream_highlight PUBLIC
  # The engine's root, because `Document.h` spells `math/MathLayout.h`.
  ${LIB_CPP_DIR}
  ${LIB_CPP_DIR}/downstream
  ${LIB_CPP_DIR}/highlight)
set_target_properties(downstream_highlight PROPERTIES
  C_STANDARD 11
  C_VISIBILITY_PRESET hidden
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

add_library(${LIB_TARGET_NAME} OBJECT
  ${LIB_CODEGEN_SRCS}
  ${LIB_ENGINE_SRCS}
  ${LIB_THEME_SRCS}
  ${LIB_MATH_SRCS}
  ${LIB_JNI_SRCS})

# The local ComponentDescriptors.h must shadow the generated one.
target_include_directories(${LIB_TARGET_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  # The engine's own root, so a header can spell another as `theme/Theme.h`
  # the way the sources read, whatever directory it sits in.
  ${LIB_CPP_DIR}
  ${LIB_CPP_DIR}/downstream
  ${LIB_CPP_DIR}/session
  ${LIB_CPP_DIR}/fabric
  ${LIB_CPP_DIR}/remend
  ${LIB_CPP_DIR}/highlight
  ${LIB_CPP_DIR}/theme
  ${LIB_CPP_DIR}/math
  ${LIB_GENERATED_JNI_DIR}
  ${LIB_GENERATED_COMPONENTS_DIR})

target_compile_definitions(${LIB_TARGET_NAME} PRIVATE MD4C_USE_UTF8=1 ${MD4C_SYMBOL_PREFIXES})

target_link_libraries(${LIB_TARGET_NAME}
  downstream_md4c
  downstream_highlight
  # dlopen, for the math package's engine, which is found at run time.
  dl
  fbjni
  jsi
  reactnative)

target_compile_reactnative_options(${LIB_TARGET_NAME} PRIVATE)
