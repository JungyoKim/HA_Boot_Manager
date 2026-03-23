#!/bin/bash
# HA Boot Manager v9.0 설치 스크립트
# 다른 컴퓨터에 이식할 때 사용
#
# 사용법:
#   chmod +x install.sh
#   sudo ./install.sh
#
# 설치 후 첫 부팅에서 's' 키를 눌러 Setup 화면에서 서버 IP/포트를 설정하세요.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUNDLE_DIR="$SCRIPT_DIR/bundle"
ESP_PATH=""

# ============================================================================
# 색상
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
fail() { echo -e "  ${RED}✗${NC} $1"; }
info() { echo -e "  ${CYAN}→${NC} $1"; }
warn() { echo -e "  ${YELLOW}!${NC} $1"; }

# ============================================================================
# Root 확인
# ============================================================================
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}root 권한이 필요합니다. sudo로 실행해주세요.${NC}"
    echo "  sudo $0"
    exit 1
fi

echo ""
echo -e "${CYAN}==========================================${NC}"
echo -e "${CYAN}  HA Boot Manager v9.0 설치${NC}"
echo -e "${CYAN}==========================================${NC}"
echo ""

# ============================================================================
# 1. ESP 경로 찾기
# ============================================================================
echo "[1/5] ESP(EFI System Partition) 탐색..."

if mountpoint -q /boot/efi 2>/dev/null; then
    ESP_PATH="/boot/efi"
elif mountpoint -q /efi 2>/dev/null; then
    ESP_PATH="/efi"
else
    # ESP를 찾아서 마운트
    ESP_DEV=$(lsblk -nro NAME,PARTTYPE | grep -i 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b' | head -1 | awk '{print $1}')
    if [ -n "$ESP_DEV" ]; then
        ESP_PATH="/boot/efi"
        mkdir -p "$ESP_PATH"
        mount "/dev/$ESP_DEV" "$ESP_PATH"
        warn "ESP를 $ESP_PATH 에 마운트했습니다 (/dev/$ESP_DEV)"
    else
        fail "ESP를 찾을 수 없습니다."
        echo "  수동으로 지정: sudo $0 --esp /path/to/esp"
        exit 1
    fi
fi

ok "ESP 경로: $ESP_PATH"

# ============================================================================
# 2. 번들 파일 확인
# ============================================================================
echo ""
echo "[2/5] 설치 파일 확인..."

if [ ! -f "$BUNDLE_DIR/HaBootManager.efi" ]; then
    fail "bundle/HaBootManager.efi 가 없습니다."
    echo "  먼저 bundle.sh 로 번들을 생성하거나, bundle/ 디렉토리를 확인하세요."
    exit 1
fi

ok "HaBootManager.efi ($(du -h "$BUNDLE_DIR/HaBootManager.efi" | awk '{print $1}'))"

DRIVER_COUNT=0
if [ -d "$BUNDLE_DIR/drivers" ]; then
    DRIVER_COUNT=$(ls "$BUNDLE_DIR/drivers/"*.efi 2>/dev/null | wc -l)
fi
ok "네트워크 드라이버: ${DRIVER_COUNT}개"

# ============================================================================
# 3. 파일 복사
# ============================================================================
echo ""
echo "[3/5] EFI 파일 복사..."

mkdir -p "$ESP_PATH/EFI/HaBootManager/drivers"

# 기존 파일 백업
if [ -f "$ESP_PATH/EFI/HaBootManager/HaBootManager.efi" ]; then
    cp "$ESP_PATH/EFI/HaBootManager/HaBootManager.efi" \
       "$ESP_PATH/EFI/HaBootManager/HaBootManager.efi.bak"
    warn "기존 파일 백업: HaBootManager.efi.bak"
fi

cp "$BUNDLE_DIR/HaBootManager.efi" "$ESP_PATH/EFI/HaBootManager/"
ok "HaBootManager.efi"

if [ -d "$BUNDLE_DIR/drivers" ]; then
    for drv in "$BUNDLE_DIR/drivers/"*.efi; do
        if [ -f "$drv" ]; then
            cp "$drv" "$ESP_PATH/EFI/HaBootManager/drivers/"
            ok "$(basename "$drv")"
        fi
    done
fi

# ============================================================================
# 4. UEFI 부트 엔트리 등록
# ============================================================================
echo ""
echo "[4/5] UEFI 부트 엔트리 설정..."

# ESP가 어떤 디스크/파티션인지 확인
ESP_MOUNT_DEV=$(findmnt -n -o SOURCE "$ESP_PATH" | head -1)

if [ -z "$ESP_MOUNT_DEV" ]; then
    warn "ESP 디바이스를 찾을 수 없습니다. 수동으로 부트 엔트리를 추가하세요."
else
    # /dev/nvme0n1p3 → disk=/dev/nvme0n1, part=3
    # /dev/sda1 → disk=/dev/sda, part=1
    if [[ "$ESP_MOUNT_DEV" =~ (nvme[0-9]+n[0-9]+)p([0-9]+) ]]; then
        DISK="/dev/${BASH_REMATCH[1]}"
        PART="${BASH_REMATCH[2]}"
    elif [[ "$ESP_MOUNT_DEV" =~ (sd[a-z]+)([0-9]+) ]]; then
        DISK="/dev/${BASH_REMATCH[1]}"
        PART="${BASH_REMATCH[2]}"
    elif [[ "$ESP_MOUNT_DEV" =~ (vd[a-z]+)([0-9]+) ]]; then
        DISK="/dev/${BASH_REMATCH[1]}"
        PART="${BASH_REMATCH[2]}"
    else
        DISK=""
        PART=""
    fi

    # 이미 등록되어 있는지 확인
    EXISTING=$(efibootmgr 2>/dev/null | grep -i "HA.*Boot.*Manager" | head -1 | grep -oP 'Boot\K[0-9A-Fa-f]{4}')

    if [ -n "$EXISTING" ]; then
        ok "기존 부트 엔트리 발견: Boot${EXISTING}"
    elif [ -n "$DISK" ] && [ -n "$PART" ]; then
        efibootmgr -c -d "$DISK" -p "$PART" \
            -L "HA Boot Manager" \
            -l '\EFI\HaBootManager\HaBootManager.efi' \
            > /dev/null 2>&1
        ok "부트 엔트리 등록 완료 (디스크: $DISK, 파티션: $PART)"

        # 방금 만든 엔트리 번호 찾기
        EXISTING=$(efibootmgr 2>/dev/null | grep -i "HA.*Boot.*Manager" | head -1 | grep -oP 'Boot\K[0-9A-Fa-f]{4}')
    else
        warn "디스크 구조를 파악할 수 없습니다. 수동으로 등록하세요:"
        info "efibootmgr -c -d /dev/sdX -p N -L 'HA Boot Manager' -l '\\EFI\\HaBootManager\\HaBootManager.efi'"
    fi

    # 1순위로 설정
    if [ -n "$EXISTING" ]; then
        CURRENT_ORDER=$(efibootmgr 2>/dev/null | grep "^BootOrder:" | awk '{print $2}')
        # 현재 순서에서 해당 엔트리 제거 후 맨 앞에 추가
        NEW_ORDER=$(echo "$CURRENT_ORDER" | tr ',' '\n' | grep -iv "$EXISTING" | tr '\n' ',' | sed 's/,$//')
        if [ -n "$NEW_ORDER" ]; then
            NEW_ORDER="${EXISTING},${NEW_ORDER}"
        else
            NEW_ORDER="${EXISTING}"
        fi
        efibootmgr -o "$NEW_ORDER" > /dev/null 2>&1
        ok "부트 순서 1순위로 설정: Boot${EXISTING}"
    fi
fi

# ============================================================================
# 5. 완료
# ============================================================================
echo ""
echo -e "${CYAN}==========================================${NC}"
echo -e "${GREEN}  설치 완료!${NC}"
echo -e "${CYAN}==========================================${NC}"
echo ""
echo "  다음 단계:"
echo "    1. 재부팅"
echo "    2. 부팅 시 's' 키를 눌러 Setup 진입"
echo "    3. Server IP, Port, 네트워크 설정"
echo "    4. Save & Exit"
echo ""
echo "  부트 순서 확인:"
echo "    efibootmgr -v"
echo ""
echo "  복구 방법:"
echo "    - Setup 타임아웃 후 자동으로 다음 부트 엔트리로 넘어갑니다"
echo "    - 백업 복원: cp $ESP_PATH/EFI/HaBootManager/HaBootManager.efi.bak"
echo "                    $ESP_PATH/EFI/HaBootManager/HaBootManager.efi"
echo ""
