# HA Boot Manager

Home Assistant와 연동되는 UEFI 부트 매니저입니다.

## 개요

HaBootManager는 UEFI 애플리케이션으로, 다음과 같이 동작합니다:
1. ESP에서 부팅 가능한 OS를 자동 탐지 (Windows, Ubuntu, Fedora 등)
2. Home Assistant의 TCP 서버에 연결하여 부팅할 OS 선택을 수신
3. 선택된 OS를 부팅하거나, 실패 시 기본 OS로 폴백

Home Assistant를 통해 원격으로 여러 머신의 부팅 OS를 선택할 수 있습니다.

## 구성 요소

### UEFI 애플리케이션 (`efi/`)
- EDK2 프레임워크 기반 C 언어로 작성
- NVRAM 영구 설정 (서버 IP, 포트, 네트워크 모드, 타임아웃, 기본 OS)
- 부팅 카운트다운 중 `S` 키로 Setup TUI 진입
- TCP 클라이언트: 탐지된 OS 목록 전송 및 부팅 선택 수신
- DHCP / 고정 IP 지원
- GOP 최대 해상도 자동 감지

### Home Assistant 커스텀 통합 (`custom_components/ha_boot_manager/`)
- Config Flow 기반 설정
- **Select 엔티티** - 다음 부팅할 OS 선택 드롭다운
- **Status 센서** - 온라인/오프라인 상태, 마지막 부팅 시간 및 클라이언트 IP 표시
- **TCP 서버** - UEFI 클라이언트 연결 수신 대기
- 부팅 요청 후 자동으로 "Menu"로 초기화
- 서비스: `ha_boot_manager.set_boot_os` (자동화용)

### 배포 스크립트
- `install.sh` - ESP 자동 감지, EFI 바이너리 + 드라이버 복사, 부팅 항목 등록 및 최우선 순위 설정
- `bundle.sh` - 이식 가능한 배포 번들 생성 (tar.gz)

## 빌드

### 필수 조건
- EDK2 툴체인
- GCC

### UEFI 애플리케이션 빌드
```bash
cd edk2
source edksetup.sh
build -a X64 -t GCC -p HaBootManagerPkg/HaBootManagerPkg.dsc -b DEBUG
```

결과물: `edk2/Build/HaBootManagerPkg/DEBUG_GCC/X64/HaBootManager.efi`

### ESP에 설치
```bash
sudo ./install.sh
```

### HA 통합 배포
`custom_components/ha_boot_manager/` 디렉토리를 Home Assistant의 `config/custom_components/`에 복사한 후 HA를 재시작합니다.

## 설정

### UEFI Setup (부팅 시 `S` 키)
| 항목 | 설명 |
|------|------|
| Server IP | Home Assistant 서버 IP 주소 |
| Server Port | TCP 포트 (기본값: 7788) |
| Network Mode | DHCP 또는 Static |
| Static IP / Subnet / Gateway | 고정 IP 네트워크 설정 |
| Boot Timeout | Setup 키 대기 시간 (초) |
| Default OS | 서버 연결 실패 시 부팅할 OS |

### Home Assistant
설정 > 기기 및 서비스 > 통합 추가 > HA Boot Manager로 추가합니다.

설정 과정에서 TCP 포트와 기본 OS를 지정합니다.

## 지원 OS 자동 탐지
- Windows
- Ubuntu
- Fedora
- CentOS
- Debian
- Arch Linux
- Manjaro
- openSUSE
- Pop!_OS
- Linux Mint
- systemd-boot

## 아키텍처

```
[UEFI 부팅] --> [HaBootManager.efi]
                    |
                    +--> ESP에서 부팅 가능한 OS 탐지
                    +--> Setup TUI 표시 ('S' 키 입력 시)
                    +--> HA TCP 서버에 연결
                    |       |
                    |       +--> OS 목록 전송
                    |       +--> 부팅 선택 수신
                    |
                    +--> 선택된 OS 부팅
                    +--> 실패 시 기본 OS로 폴백

[Home Assistant] --> [ha_boot_manager 통합]
                        |
                        +--> TCP 서버 (UEFI 클라이언트 수신 대기)
                        +--> Select 엔티티 (OS 선택 드롭다운)
                        +--> Status 센서 (온라인/오프라인)
                        +--> 부팅 후 자동으로 "Menu"로 초기화
```

## 팁
- GRUB 메뉴 건너뛰기: `/etc/default/grub`에서 `GRUB_TIMEOUT=0`, `GRUB_TIMEOUT_STYLE=hidden`, `GRUB_DISABLE_OS_PROBER=true` 설정 후 `sudo update-grub` 실행
- `install.sh`를 사용하여 여러 머신에 간편하게 배포 가능
