#!/bin/bash
# HA Boot Manager - 배포 번들 생성
# EFI 바이너리 + 드라이버를 bundle/ 디렉토리로 패키징
#
# 사용법:
#   ./bundle.sh          # 현재 빌드 결과물 + 현재 머신의 드라이버
#   ./bundle.sh --build  # EDK2 빌드부터 수행

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EDK2_DIR="$SCRIPT_DIR/edk2"
BUILD_DIR="$EDK2_DIR/Build/HaBootManagerPkg/DEBUG_GCC/X64"
BUNDLE_DIR="$SCRIPT_DIR/bundle"
ESP_DRIVERS="/boot/efi/EFI/HaBootManager/drivers"

echo "==========================================="
echo "  HA Boot Manager 번들 생성"
echo "==========================================="

# 빌드 옵션
if [ "$1" = "--build" ]; then
    echo ""
    echo "[빌드] EDK2 빌드 시작..."
    cd "$EDK2_DIR"
    source edksetup.sh
    build -a X64 -t GCC -p HaBootManagerPkg/HaBootManagerPkg.dsc -b DEBUG
    cd "$SCRIPT_DIR"
    echo ""
fi

# 번들 디렉토리 생성
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/drivers"

# EFI 바이너리 복사
if [ -f "$BUILD_DIR/HaBootManager.efi" ]; then
    cp "$BUILD_DIR/HaBootManager.efi" "$BUNDLE_DIR/"
    echo "✓ HaBootManager.efi ($(du -h "$BUILD_DIR/HaBootManager.efi" | awk '{print $1}'))"
else
    echo "✗ HaBootManager.efi 없음 - --build 옵션으로 빌드하세요"
    exit 1
fi

# 드라이버 복사 (현재 머신의 ESP에서)
if [ -d "$ESP_DRIVERS" ]; then
    for drv in "$ESP_DRIVERS/"*.efi; do
        if [ -f "$drv" ]; then
            cp "$drv" "$BUNDLE_DIR/drivers/"
            echo "✓ drivers/$(basename "$drv")"
        fi
    done
else
    echo "! ESP 드라이버 디렉토리 없음, drivers/ 비어있음"
fi

# install.sh 복사
cp "$SCRIPT_DIR/install.sh" "$BUNDLE_DIR/"
chmod +x "$BUNDLE_DIR/install.sh"

echo ""
echo "==========================================="
echo "  번들 생성 완료: $BUNDLE_DIR/"
echo "==========================================="
echo ""
echo "  배포 방법:"
echo "    1. bundle/ 폴더를 대상 머신에 복사"
echo "    2. sudo ./bundle/install.sh"
echo ""

# 압축 파일 생성
ARCHIVE="$SCRIPT_DIR/ha-boot-manager-v9.tar.gz"
tar -czf "$ARCHIVE" -C "$SCRIPT_DIR" bundle/
echo "  압축 파일: $ARCHIVE ($(du -h "$ARCHIVE" | awk '{print $1}'))"
echo "  USB로 복사 후: tar xzf ha-boot-manager-v9.tar.gz && sudo ./bundle/install.sh"
echo ""
