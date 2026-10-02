require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "DownstreamMath"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported }
  s.source       = { :git => package["repository"]["url"].sub(/^git\+/, ""), :tag => "math-v#{s.version}" }

  # The installer, which hands RaTeX's three C functions and the font directory
  # to Downstream from `+load`. Nothing else here is compiled.
  s.source_files = "ios/*.{h,m,mm}"
  s.dependency "Downstream"

  # RaTeX's static XCFramework, fetched by scripts/vendor-ratex.mjs from the
  # release pinned in ratex-version.json. Only the iOS slices are vendored.
  s.vendored_frameworks = "vendor/RaTeX.xcframework"
  # The KaTeX fonts, in a bundle of their own so their names cannot collide with
  # an app's resources. They are read from this directory, never registered.
  s.resource_bundles = { "DownstreamMathFonts" => ["vendor/fonts/*.ttf"] }
  s.preserve_paths = "ratex-version.json", "licenses/*"

  s.pod_target_xcconfig = { "CLANG_CXX_LANGUAGE_STANDARD" => "c++20" }

  unless File.exist?(File.join(__dir__, "vendor", "RaTeX.xcframework", "Info.plist"))
    raise "DownstreamMath: RaTeX is not vendored. Run `node #{File.join(__dir__, "scripts", "vendor-ratex.mjs")}`."
  end
end
