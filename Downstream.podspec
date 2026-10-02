require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

MD4C_SYMBOL_PREFIXES = "md_parse=downstream_md_parse entity_lookup=downstream_entity_lookup"
# An app may link its own Scintilla or Lexilla, and both projects rename their
# namespaces with a preprocessor define, so our vendored copy does too.
LEXILLA_SYMBOL_PREFIXES = "Lexilla=downstream_lexilla Scintilla=downstream_scintilla"

Pod::Spec.new do |s|
  s.name         = "Downstream"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported }
  s.source       = { :git => package["repository"]["url"].sub(/^git\+/, ""), :tag => "v#{s.version}" }

  s.source_files = [
    "src/ios/**/*.{h,m,mm}",
    "src/cpp/downstream/*.{h,cpp}",
    "src/cpp/session/*.{h,cpp}",
    "src/cpp/fabric/*.{h,cpp}",
    "src/cpp/remend/*.{h,cpp}",
    "src/cpp/highlight/*.{h,cpp}",
    "src/cpp/theme/*.{h,cpp}",
    # Math: the sizing, the display-list decoder, and the block pass. Listed
    # because it is its own directory rather than a subdirectory of `downstream`,
    # and a source file the pod does not ship is a link error at install time.
    "src/cpp/math/*.{h,cpp}",
    "src/cpp/lexilla/*.{h,cxx}",
    "src/cpp/lexilla/lexers/*.cxx",
    "src/cpp/lexilla/lexlib/*.{h,cxx}",
    "src/cpp/md4c/md4c.{c,h}",
    "src/cpp/md4c/entity.{c,h}",
    # The vendored lexers are HPND-licensed and their notice has to ship.
    "src/cpp/lexilla/License.txt",
  ]
  s.private_header_files = "src/ios/**/*.h", "src/cpp/**/*.h"

  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => [
      # The engine's own root, so a header can spell another as
      # `theme/Theme.h` the way the sources read, whatever directory it sits in.
      "\"$(PODS_TARGET_SRCROOT)/src/cpp\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/downstream\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/session\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/fabric\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/md4c\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/remend\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/highlight\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/theme\"",
      # `downstream/PreparedDocument.cpp` spells the math headers as
      # `MathBlocks.h`, so the directory itself has to be on the path and not only
      # its parent.
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/math\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/lexilla\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/lexilla/lexlib\"",
    ].join(" "),
    # Other libraries, such as Enriched Markdown, link their own md4c, and an
    # app may link its own Scintilla. Prefixing ours keeps the linker from
    # binding our calls to theirs.
    "GCC_PREPROCESSOR_DEFINITIONS" =>
      "$(inherited) MD4C_USE_UTF8=1 #{MD4C_SYMBOL_PREFIXES} #{LEXILLA_SYMBOL_PREFIXES}",
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
  }

  install_modules_dependencies(s)
end
