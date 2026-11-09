# Colorful CVN X570M ARGB Control

Colorful **CVN X570M GAMING FROZEN V14** 메인보드의 RGB/ARGB를 리눅스에서 제어하는 비공식 도구입니다.
Windows의 iGame Center 없이 색상과 효과를 설정할 수 있고, GALAX RTX 3070의 LED도 같은 효과로 동기화할 수 있습니다.

> 제조사와 관련 없는 비공식 프로젝트입니다. 하드웨어에 직접 명령을 보내므로 사용에 따른 책임은 사용자에게 있습니다.

## 주요 기능

- GTK4/libadwaita 설정 창
- 효과 13종: 고정, 호흡, 깜빡임, 색상 순환, 무지개, 별빛, 별똥별, 샤랄라, 유성비, 번개, 음악 반응, 수면, CPU 온도
- 효과별 색상 저장, 속도, 밝기, 방향, 음악 감도, 온도 범위, 색상 순서 설정
- 구역별 켜기/끄기, LED 수, 역방향, 개별 색상 설정
- 로그인 시 자동 적용, 절전 복귀 후 재연결
- 선택 사항: GALAX RTX 3070 LED 동기화

| 구역 | LED 수 | 슬롯 |
|---|---|---|
| 12V 헤더 1, 2 | 각 1 | 0, 1 |
| 보드 LED | 18 | 2-19 |
| 5V ARGB 1, 2, 3 | 각 최대 60 | 20-79, 80-139, 140-199 |
| 그래픽카드 (선택) | 1 | 별도 |

5V 헤더의 LED 수는 기본값이 60개입니다. 연결한 팬이나 스트립의 실제 LED 수로 맞춰야 효과가 자연스럽습니다. 데이지체인으로 연결했다면 전부 더한 값을 넣으세요.

## 테스트 환경

| 항목 | 내용 |
|---|---|
| 메인보드 | Colorful CVN X570M GAMING FROZEN V14 |
| 컨트롤러 | USB HID `2f4c:1000` (usage page `0xff01`) |
| 그래픽카드 | GALAX RTX 3070 (`10de:2484`) |
| OS | Arch Linux, GNOME (Wayland) |

다른 보드나 그래픽카드에서는 테스트하지 않았습니다.

## 설치

### 1. 의존성

Arch Linux 기준입니다.

```bash
sudo pacman -S --needed base-devel gtk4 libadwaita glib2 libpulse
```

음악 반응 효과에는 PipeWire(`pipewire-pulse`) 또는 PulseAudio가 필요합니다.

### 2. 빌드와 설치

```bash
make
make install
```

`make install`은 사용자 영역에만 설치하고 상주 서비스를 켭니다.

| 파일 | 위치 |
|---|---|
| 실행 파일 | `~/.local/bin/` |
| 상주 서비스 | `~/.config/systemd/user/colorful-rgbd.service` |
| 앱 메뉴 항목 | `~/.local/share/applications/colorful-rgb.desktop` |

### 3. 장치 권한 (필수)

로그인한 사용자가 LED 제어 인터페이스에 쓸 수 있게 합니다. 키보드 인터페이스에는 권한을 주지 않습니다.

```bash
sudo install -m644 70-colorful-rgb.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
```

### 4. 그래픽카드 동기화 (선택)

GALAX RTX 3070을 함께 쓸 때만 설치합니다. 빌드는 일반 사용자로 먼저 해 두어야 합니다.

```bash
sudo make install-gpu
```

GPU의 I2C 버스는 root 권한이 필요해서, 색상 값만 전달받는 작은 root 서비스를 따로 둡니다.

- 소켓은 설치한 사용자만 접근할 수 있습니다.
- 첫 번째 NVIDIA I2C 어댑터의 주소 `0x23`에서 식별값 `0x30`을 확인하고, 다르면 아무것도 쓰지 않고 종료합니다.
- `0x02`-`0x06` 레지스터만 쓰며, 저장 레지스터는 쓰지 않아 카드에 영구 저장되지 않습니다.

식별값이 같아도 다른 카드에서 안전하다는 보장은 없습니다. RTX 3070 외의 카드에서는 내용을 이해한 경우에만 사용하세요.

## 사용법

앱 메뉴에서 **Colorful RGB**를 실행하거나 터미널에서 `colorful-rgb-gui`를 실행합니다.

설정은 `~/.config/colorful-rgb/config.ini`에 저장되고, 상주 프로그램이 바로 반영합니다.

### 전원과 색상 유지

컨트롤러는 대기 전원이 있는 동안 마지막 프레임을 기억합니다.

- **재부팅:** 색이 유지됩니다. 움직이는 효과는 로그인할 때까지 멈춰 보입니다.
- **전원 완전 차단:** 컨트롤러가 초기화되고, 로그인하면 저장된 설정이 다시 적용됩니다.

### CLI

단색만 설정하는 Python 도구도 포함되어 있습니다. 표준 라이브러리만 사용합니다.

```bash
./colorful-rgb ff0080 --brightness 60
./colorful-rgb off
./colorful-rgb red --dry-run
```

상주 서비스가 켜져 있으면 곧바로 덮어쓰므로, 먼저 `systemctl --user stop colorful-rgbd`를 실행하세요.

## 구성

```text
colorful-rgb-gui ──(config.ini)──> colorful-rgbd ──(hidraw)──> 메인보드 컨트롤러
                                        │
                                        └──(유닉스 소켓)──> colorful-gpu-ledd ──(I2C)──> 그래픽카드
```

| 파일 | 역할 |
|---|---|
| `colorful-rgb-gui.c` | 설정 창 |
| `colorful-rgbd.c` | 효과 계산과 전송 (초당 30프레임) |
| `colorful-gpu-ledd.c` | 그래픽카드 LED에 색을 쓰는 root 서비스 |
| `rgb-core.c`, `rgb-core.h` | 설정 파일과 메인보드 장치 처리 |
| `colorful-rgb` | 단색 설정용 Python CLI |

## 프로토콜

### 메인보드

한 프레임은 데이터 리포트 10개와 커밋 리포트 1개입니다. 각 리포트는 맨 앞의 report ID를 포함해 65바이트입니다.

```text
데이터: 00 01 00 88 <페이지 0-9> <LED 20개 x 3바이트>
커밋:   00 01 00 88 ff <0으로 채움>
```

컨트롤러에 내장된 효과는 사용하지 않고, 모든 효과는 PC에서 프레임을 계산해 전송합니다.

### 그래픽카드

I2C 주소 `0x23`, SMBus byte-data 방식입니다.

| 레지스터 | 용도 |
|---|---|
| `0x00` | 식별값 (`0x30`) |
| `0x02`, `0x03`, `0x04` | 빨강, 초록, 파랑 |
| `0x05`, `0x06` | 모드 (`00 20` = 고정, `04 20` = 호흡) |

## 제거

```bash
make uninstall
sudo make uninstall-gpu
sudo rm /etc/udev/rules.d/70-colorful-rgb.rules
```

## 참고

- 그래픽카드 레지스터 정보는 [OpenRGB의 GALAX RTX 30 시리즈 작업](https://gitlab.com/CalcProgrammer1/OpenRGB/-/merge_requests/2938)을 참고해, 실제 카드에서 값을 대조했습니다.
- 효과 코드는 이 프로젝트에서 새로 작성했으며, 제조사 소프트웨어의 코드나 펌웨어를 포함하지 않습니다.
