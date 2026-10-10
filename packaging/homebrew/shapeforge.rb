# Homebrew formula template. Put it in a tap repo (github.com/rashid5925/homebrew-tap,
# file Formula/shapeforge.rb) and update version/sha256 on each release.
# Users then run:  brew install rashid5925/tap/shapeforge
class Shapeforge < Formula
  desc "3D modeling tools for AI assistants (MCP server): build, render, measure, export OBJ"
  homepage "https://github.com/rashid5925/3d-model-generator"
  version "0.2.0"
  license "MIT"

  on_macos do
    url "https://github.com/rashid5925/3d-model-generator/releases/download/v#{version}/shapeforge-macos-universal"
    sha256 "REPLACE_WITH_SHA256_FROM_SHA256SUMS"
  end
  on_linux do
    on_intel do
      url "https://github.com/rashid5925/3d-model-generator/releases/download/v#{version}/shapeforge-linux-x64"
      sha256 "REPLACE_WITH_SHA256_FROM_SHA256SUMS"
    end
    on_arm do
      url "https://github.com/rashid5925/3d-model-generator/releases/download/v#{version}/shapeforge-linux-arm64"
      sha256 "REPLACE_WITH_SHA256_FROM_SHA256SUMS"
    end
  end

  def install
    bin.install Dir["shapeforge-*"].first => "shapeforge"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/shapeforge version")
  end
end
