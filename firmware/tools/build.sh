#!/usr/bin/env bash
# Build Mu-Silicium UEFI for the Galaxy S20 FE (r8s) with the r8s-woa overlay.
# Works on Linux, WSL and GitHub Actions (ubuntu-latest).
#
#   ./firmware/tools/build.sh [-w WORKDIR] [-r MU_REF] [-t RELEASE|DEBUG] [--no-deps]
#
# Output: out/Mu-r8s.img (Android boot image, flash to BOOT)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKDIR="${WORKDIR:-$ROOT/work}"
MU_REPO="${MU_REPO:-https://github.com/Project-Silicium/Mu-Silicium.git}"
MU_REF="${MU_REF:-main}"
TARGET="RELEASE"
INSTALL_DEPS=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    -w) WORKDIR="$2"; shift 2 ;;
    -r) MU_REF="$2"; shift 2 ;;
    -t) TARGET="$2"; shift 2 ;;
    --no-deps) INSTALL_DEPS=0; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

MU="$WORKDIR/Mu-Silicium"
mkdir -p "$WORKDIR" "$ROOT/out"

if [[ ! -d "$MU/.git" ]]; then
  echo "==> Cloning Mu-Silicium ($MU_REF, ~5 GB with submodules)"
  git clone --recursive "$MU_REPO" "$MU"
fi

echo "==> Checking out $MU_REF"
git -C "$MU" fetch --all --tags --quiet
git -C "$MU" checkout --quiet "$MU_REF"
git -C "$MU" submodule update --init --recursive

echo "==> Resetting r8s files to upstream before applying overlay"
git -C "$MU" checkout -- Platforms/Samsung/r8sPkg
rm -rf "$MU/Silicium-ACPI/Platforms/Samsung/r8s"

echo "==> Applying r8s-woa overlay"
cp -rv "$ROOT/firmware/overlay/." "$MU/"

if [[ $INSTALL_DEPS -eq 1 ]]; then
  echo "==> Installing build dependencies"
  (cd "$MU" && ./setup_env.sh -p apt)
fi

echo "==> Building r8s ($TARGET)"
(cd "$MU" && python3 build_uefi.py -d r8s -r "$TARGET")

cp -v "$MU/Mu-r8s.img" "$ROOT/out/Mu-r8s.img"

echo "==> Packing Odin tar (AP slot, writes BOOT)"
ODIN_DIR="$(mktemp -d)"
cp "$MU/Mu-r8s.img" "$ODIN_DIR/boot.img"
tar --format=ustar -C "$ODIN_DIR" -cf "$ROOT/out/Mu-r8s-odin.tar" boot.img
(cd "$ROOT/out" && md5sum Mu-r8s-odin.tar >> Mu-r8s-odin.tar && mv Mu-r8s-odin.tar Mu-r8s-odin.tar.md5)
rm -rf "$ODIN_DIR"

echo "==> Building the ESP test image (UEFI Shell on FAT32)"
if ! command -v mkfs.fat >/dev/null || ! command -v mcopy >/dev/null; then
  sudo apt-get install -y dosfstools mtools
fi
SHELL_EFI="$(find "$MU/Build/r8sPkg/${TARGET}_CLANGPDB/AARCH64" -maxdepth 1 -name Shell.efi | head -n1)"
DIAG_EFI="$(find "$MU/Build/r8sPkg/${TARGET}_CLANGPDB/AARCH64" -maxdepth 1 -name R8sDiag.efi | head -n1)"
TRACE_EFI="$(find "$MU/Build/r8sPkg/${TARGET}_CLANGPDB/AARCH64" -maxdepth 1 -name R8sTrace.efi | head -n1)"
"$ROOT/firmware/tools/make-esp.sh" "$SHELL_EFI" "$DIAG_EFI" "$TRACE_EFI"

(cd "$ROOT/out" && sha256sum Mu-r8s.img Mu-r8s-odin.tar.md5 esp-r8s.img.gz | tee SHA256SUMS)
echo "==> Done: $ROOT/out/Mu-r8s.img, $ROOT/out/Mu-r8s-odin.tar.md5"
