#!/usr/bin/env bash
# Build out/esp-r8s.img.gz: a 1 GiB FAT32 image (4096-byte sectors, like the
# S20 FE's UFS) holding the EDK2 UEFI Shell as \EFI\Boot\bootaa64.efi and a
# startup.nsh that tests storage on its own.
#
# Partition-Phone.ps1 writes it to the new 'esp' partition. Booting the
# phone's internal storage from the Silicium menu then starts the Shell from
# UFS, which proves UfsDxe + partition + FAT reads work.
#
# Usage: make-esp.sh <Shell.efi>   (build.sh passes the one it just built)
# Needs: dosfstools (mkfs.fat), mtools (mcopy/mmd), gzip.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/out"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

SHELL_EFI="${1:-}"
DIAG_EFI="${2:-}"
TRACE_EFI="${3:-}"
[[ -f "$SHELL_EFI" ]] || { echo "usage: make-esp.sh <Shell.efi> [R8sDiag.efi]" >&2; exit 1; }

cat > "$TMP/startup.nsh" <<'EOF'
@echo -off
echo " "
echo "=== r8s-woa storage test ==="
echo "This shell was loaded from the phone's UFS (esp partition)."
echo "Block devices and file systems the UEFI can see:"
map -r
echo " "
echo "Files on the esp partition:"
for %m in fs0 fs1 fs2 fs3
  if exist %m:\r8s-woa-esp.txt then
    %m:
    ls
    type r8s-woa-esp.txt
  endif
endfor
echo " "
echo "If you can read this and the file list above, UFS reads work."
for %m in fs0 fs1 fs2 fs3
  if exist %m:\EFI\r8s\R8sDiag.efi then
    %m:\EFI\r8s\R8sDiag.efi
  endif
endfor
echo "Type 'reset' to reboot, or 'reset -s' to power off."
EOF

echo "r8s-woa ESP test image: UFS + GPT + FAT32 read OK" > "$TMP/r8s-woa-esp.txt"

IMG="$TMP/esp-r8s.img"
truncate -s 1024M "$IMG"
mkfs.fat -F 32 -S 4096 -n ESP "$IMG" >/dev/null
export MTOOLS_SKIP_CHECK=1
mmd   -i "$IMG" ::/EFI ::/EFI/Boot
mcopy -i "$IMG" "$SHELL_EFI"        ::/EFI/Boot/bootaa64.efi
mcopy -i "$IMG" "$TMP/startup.nsh"  ::/startup.nsh
mcopy -i "$IMG" "$TMP/r8s-woa-esp.txt" ::/r8s-woa-esp.txt
if [[ -f "$DIAG_EFI" ]]; then
  mmd   -i "$IMG" ::/EFI/r8s
  mcopy -i "$IMG" "$DIAG_EFI" ::/EFI/r8s/R8sDiag.efi
fi
if [[ -f "$TRACE_EFI" ]]; then
  mmd   -i "$IMG" ::/EFI/r8s 2>/dev/null || true
  mcopy -i "$IMG" "$TRACE_EFI" ::/EFI/r8s/R8sTrace.efi
fi
mdir  -i "$IMG" -/ ::/

gzip -9 -c "$IMG" > "$OUT/esp-r8s.img.gz"
ls -l "$OUT/esp-r8s.img.gz"
