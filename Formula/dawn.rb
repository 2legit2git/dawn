class Dawn < Formula
  desc "Distraction-free writing environment with live markdown rendering"
  homepage "https://github.com/andrewmd5/dawn"
  version "VERSION"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/andrewmd5/dawn/releases/download/vVERSION/dawn-macos-arm64.tar.gz"
      sha256 "SHA256_ARM64"
    end
    on_intel do
      url "https://github.com/andrewmd5/dawn/releases/download/vVERSION/dawn-macos-x64.tar.gz"
      sha256 "SHA256_X64"
    end
  end

  def install
    bin.install "dawn"
  end

  test do
    assert_match "dawn", shell_output("#{bin}/dawn --help")
  end
end
