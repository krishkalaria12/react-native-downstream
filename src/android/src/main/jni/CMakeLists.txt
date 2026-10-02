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
file(GLOB LIB_HIGHLIGHT_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/highlight/*.cpp)
file(GLOB LIB_THEME_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/theme/*.cpp)
# Math is its own directory rather than a subdirectory of `downstream`, so it needs
# its own glob. Omitting it does not fail the configure step: the library links and
# then fails on the first undefined `downstream::math*` symbol, which reads like a
# platform problem rather than a missing source.
file(GLOB LIB_MATH_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/math/*.cpp)
file(GLOB LIB_LEXILLA_SRCS CONFIGURE_DEPENDS
  ${LIB_CPP_DIR}/lexilla/lexers/*.cxx
  ${LIB_CPP_DIR}/lexilla/lexlib/*.cxx)
file(GLOB LIB_JNI_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# Other libraries, such as Enriched Markdown, link their own md4c. Prefixed,
# hidden symbols keep the linker from binding our calls to theirs.
set(MD4C_SYMBOL_PREFIXES md_parse=downstream_md_parse entity_lookup=downstream_entity_lookup)

# An app may link its own Scintilla or Lexilla, and both projects rename their
# namespaces with a preprocessor define, so ours does too. Private to the two
# targets below; nothing else in the library may use them without the defines.
set(LEXILLA_SYMBOL_PREFIXES Lexilla=downstream_lexilla Scintilla=downstream_scintilla)

add_library(downstream_md4c STATIC
  ${LIB_CPP_DIR}/md4c/md4c.c
  ${LIB_CPP_DIR}/md4c/entity.c)
target_include_directories(downstream_md4c PUBLIC ${LIB_CPP_DIR}/md4c)
target_compile_definitions(downstream_md4c PRIVATE MD4C_USE_UTF8=1 ${MD4C_SYMBOL_PREFIXES})
set_target_properties(downstream_md4c PROPERTIES
  C_STANDARD 11
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

add_library(downstream_lexilla STATIC ${LIB_LEXILLA_SRCS})
target_include_directories(downstream_lexilla PUBLIC
  ${LIB_CPP_DIR}/lexilla
  ${LIB_CPP_DIR}/lexilla/lexlib)
target_compile_definitions(downstream_lexilla PUBLIC ${LEXILLA_SYMBOL_PREFIXES})
set_target_properties(downstream_lexilla PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

# The highlighter wraps Lexilla, so it is built apart from the rest of the
# engine to keep the namespace renames out of every other source.
add_library(downstream_highlight STATIC ${LIB_HIGHLIGHT_SRCS})
target_include_directories(downstream_highlight PUBLIC
  ${LIB_CPP_DIR}/downstream
  ${LIB_CPP_DIR}/highlight)
target_compile_definitions(downstream_highlight PUBLIC ${LEXILLA_SYMBOL_PREFIXES})
target_link_libraries(downstream_highlight PUBLIC downstream_lexilla)
set_target_properties(downstream_highlight PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
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
  fbjni
  jsi
  reactnative)

target_compile_reactnative_options(${LIB_TARGET_NAME} PRIVATE)
