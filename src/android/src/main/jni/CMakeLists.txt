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
  ${LIB_CPP_DIR}/fabric/*.cpp)
file(GLOB LIB_JNI_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# Other libraries, such as Enriched Markdown, link their own md4c. Prefixed,
# hidden symbols keep the linker from binding our calls to theirs.
set(MD4C_SYMBOL_PREFIXES md_parse=downstream_md_parse entity_lookup=downstream_entity_lookup)

# md4c-html.c is only used by the host tests in src/cpp/tests.
add_library(downstream_md4c STATIC
  ${LIB_CPP_DIR}/md4c/md4c.c
  ${LIB_CPP_DIR}/md4c/entity.c)
target_include_directories(downstream_md4c PUBLIC ${LIB_CPP_DIR}/md4c)
target_compile_definitions(downstream_md4c PRIVATE MD4C_USE_UTF8=1 ${MD4C_SYMBOL_PREFIXES})
set_target_properties(downstream_md4c PROPERTIES
  C_STANDARD 11
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

add_library(${LIB_TARGET_NAME} OBJECT
  ${LIB_CODEGEN_SRCS}
  ${LIB_ENGINE_SRCS}
  ${LIB_JNI_SRCS})

# The local ComponentDescriptors.h must shadow the generated one.
target_include_directories(${LIB_TARGET_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIB_CPP_DIR}/downstream
  ${LIB_CPP_DIR}/session
  ${LIB_CPP_DIR}/fabric
  ${LIB_GENERATED_JNI_DIR}
  ${LIB_GENERATED_COMPONENTS_DIR})

target_compile_definitions(${LIB_TARGET_NAME} PRIVATE MD4C_USE_UTF8=1 ${MD4C_SYMBOL_PREFIXES})

target_link_libraries(${LIB_TARGET_NAME}
  downstream_md4c
  fbjni
  jsi
  reactnative)

target_compile_reactnative_options(${LIB_TARGET_NAME} PRIVATE)
