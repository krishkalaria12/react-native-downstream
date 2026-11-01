require "json"
package = JSON.parse(File.read(File.join(__dir__, "package.json")))
Pod::Spec.new do |s|
  s.name = "DownstreamCode"
  s.version = package["version"]
  s.summary = package["description"]
  s.homepage = package["homepage"]
  s.license = package["license"]
  s.authors = package["author"]
  s.platforms = { :ios => min_ios_version_supported }
  s.source = { :git => package["repository"]["url"].sub(/^git\+/, ""), :tag => "code-v#{s.version}" }
  s.source_files = "ios/*.{h,m,mm}", "cpp/*.{h,c,cpp}"
  s.exclude_files = "cpp/TestInstall.cpp"
  s.preserve_paths = "cpp/wasm/**/*"
  s.private_header_files = "cpp/**/*.h"
  s.dependency "Downstream"
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "HEADER_SEARCH_PATHS" => [
      '"$(PODS_ROOT)/Headers/Public/Downstream"',
      '"$(PODS_TARGET_SRCROOT)/cpp"',
      '"$(PODS_TARGET_SRCROOT)/cpp/wasm"',
      '"$(PODS_TARGET_SRCROOT)/cpp/wasm/wasm-rt"',
      '"$(PODS_TARGET_SRCROOT)/cpp/wasm/simde"',
    ].join(" "),
  }
end
