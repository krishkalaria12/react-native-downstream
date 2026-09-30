require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

MD4C_SYMBOL_PREFIXES = "md_parse=downstream_md_parse entity_lookup=downstream_entity_lookup"

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
    "src/cpp/md4c/md4c.{c,h}",
    "src/cpp/md4c/entity.{c,h}",
  ]
  s.private_header_files = "src/ios/**/*.h", "src/cpp/**/*.h"

  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => [
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/downstream\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/session\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/fabric\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/md4c\"",
      "\"$(PODS_TARGET_SRCROOT)/src/cpp/remend\"",
    ].join(" "),
    # Other libraries, such as Enriched Markdown, link their own md4c. Prefixing
    # ours keeps the linker from binding our calls to theirs.
    "GCC_PREPROCESSOR_DEFINITIONS" => "$(inherited) MD4C_USE_UTF8=1 #{MD4C_SYMBOL_PREFIXES}",
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
  }

  install_modules_dependencies(s)
end
