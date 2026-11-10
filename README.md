# ESP32-S31-Korvo-1 microSD USB High-Speed MSC 속도 측정

> **상태**: ESP-IDF v6.1에서 빌드까지 확인했다. 보드에서는 아직 실행하지 않았다.
> 보드에서 확인할 항목은 [확인한 범위](#확인한-범위)에 정리했다.

## 사용 보드

| 항목 | 내용 |
|---|---|
| 보드 | Espressif [ESP32-S31-Korvo-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/index.html) V1.1 |
| 모듈 | ESP32-S31-WROOM-3 (플래시 16 MB, PSRAM 16 MB) |
| 사용 기능 | microSD 슬롯(4비트 SDMMC), USB 2.0 Type-A 포트(High-Speed OTG), USB-to-UART 콘솔 |
| SD 카드 | 64 GB 이상 microSDXC, exFAT (FAT32 카드도 쓸 수 있다) |
| 개발 환경 | ESP-IDF v6.1 (Windows), esp_tinyusb 2.2.0 |

## 목적

- ESP32-S31-Korvo-1의 microSD 카드를 USB 2.0 High-Speed 대용량 저장장치(MSC)로 PC에 연결한다.
- 그 상태에서 PC가 보는 카드의 순차 읽기·쓰기 속도를 측정한다.
- 64 GB 이상 SDXC 카드의 기본 파일시스템인 exFAT을 장치 쪽에서도 읽고 쓸 수 있게 한다.
- 속도가 기대보다 낮으면 SD 버스와 USB 중 어느 쪽이 병목인지 구분한다.

측정은 세 가지 방법으로 한다.

- **PC 측**: `tools/msc_bench.py`나 CrystalDiskMark로 MSC 드라이브의 순차 쓰기·읽기 속도를 잰다.
- **장치 로그**: PC가 데이터를 주고받는 동안 장치가 USB 처리량과 SD 버스 점유율을 1초마다 출력한다.
- **장치 단독**: 카드를 앱으로 가져와 SD 원시 읽기 속도와 exFAT 파일 쓰기·읽기 속도를 잰다.
  USB를 거치지 않으므로 SD 버스 한계와 USB 한계를 나눠 볼 수 있다.

## 보드 연결

| 항목 | 내용 |
|---|---|
| microSD | SDMMC 슬롯 0, 4비트. CLK GPIO24, CMD GPIO25, D0~D3 GPIO20~23 (IO MUX 고정 핀) |
| SD_CTRL | GPIO39 (카드 전원 제어). 기본 설정에서는 건드리지 않는다 |
| USB MSC | USB 2.0 Type-A 포트 (ESP32-S31 High-Speed OTG, UTMI PHY) |
| 콘솔 | USB-to-UART Type-C 포트 (UART0, GPIO58/59), 115200 bps |

### 연결 순서

1. Type-C UART 포트를 PC에 연결한다. 전원, 플래시, 콘솔을 모두 이 포트로 쓴다.
2. USB 2.0 Type-A 포트를 PC의 USB 포트에 연결한다. Windows에서 이동식 디스크로 잡힌다.
3. 전원 스위치를 ON 쪽으로 민다.

> **주의: Type-A 포트의 VBUS**
>
> 이 포트는 원래 USB 호스트용이다.
> 사용자 가이드에 따르면 보드가 TPS2051C 전원 스위치를 거쳐 이 포트로 5 V(최대 500 mA)를 내보낸다.
> A-A 케이블이나 A-C 케이블로 PC에 바로 연결하면 PC의 VBUS와 보드의 5 V 출력이 서로 연결된다.
> VBUS 선을 끊은 케이블이나 VBUS 차단 어댑터를 쓴다.
> 케이블 VBUS를 끊어도 보드 쪽 포트에는 보드 자체 5 V가 남아 있다.
> 그래서 USB 연결에는 영향이 없을 것으로 본다.
> TPS2051C의 EN 제어 방식과 역전류 차단 여부는 회로도(esp32-s31-korvo-1-schematics.pdf)에서 확인해야 한다.
> 이 프로젝트를 만들 때는 회로도를 확인하지 못했다.

## 파일 구성

```
CMakeLists.txt            프로젝트 (MINIMAL_BUILD)
sdkconfig.defaults        보드, 콘솔, PSRAM, TinyUSB, FATFS 기본 설정
main/
  app_main.c              초기화 순서, 콘솔 REPL
  sd_card.c               SDMMC 슬롯 0 초기화, 속도 모드, 카드 정보 출력
  msc_ctrl.c              TinyUSB MSC, 카드 소유권 전환, 처리량 통계, 링커 래퍼
  bench.c                 rawread, filebench, format, 파일시스템 정보
  cmd_bench.c             콘솔 명령 등록
  Kconfig.projbuild       menuconfig "S31 SD MSC benchmark"
  idf_component.yml       espressif/esp_tinyusb ^2.2.0
components/fatfs/         ESP-IDF v6.1 fatfs 복사본 (exFAT 켬)
tools/msc_bench.py        PC 측 순차 쓰기·읽기 측정 (Windows)
```

## 빌드

PC에 설치된 ESP-IDF v6.1(`C:\esp\v6.1\esp-idf`)을 쓴다.
ESP32-S31은 IDF v6.1에서 프리뷰 타깃이므로 `idf.py`에 `--preview`를 붙인다.

PowerShell에서 다음 순서로 실행한다.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\work\ESP32\ESP32S31-SDCARD
idf.py --preview build
idf.py --preview -p COM4 flash monitor
```

- COM 번호는 Type-C UART 포트의 번호로 바꾼다.
- `sdkconfig`를 지웠다면 먼저 `idf.py --preview set-target esp32s31`을 실행한다.
  이때 `sdkconfig.defaults`로 설정이 다시 만들어진다.
- 첫 빌드에서 컴포넌트 매니저가 esp_tinyusb 2.2.0과 tinyusb 0.19.0~3을 `managed_components`에 받는다.

## menuconfig 설정

`idf.py --preview menuconfig`의 `S31 SD MSC benchmark` 메뉴에 있다.

| 항목 | 기본값 | 설명 |
|---|---|---|
| SD bus speed mode | High Speed 40 MHz | Default Speed 20 MHz, High Speed 40 MHz, High Speed 50 MHz(실험), UHS-I SDR50 100 MHz(실험) |
| SD bus width | 4 lines | 1비트로 바꿀 수 있다 |
| Enable internal pull-ups | 켬 | 보드 풀업에 내부 풀업을 더한다 (IDF 예제와 같다) |
| On-chip LDO channel | 1 | SDR50에서만 쓴다. IDF sd_card 예제의 ESP32-S31 값과 같다 |
| Power-cycle the card with a GPIO | 끔 | GPIO39로 카드 전원을 껐다 켠다. 극성을 회로도에서 확인한 뒤 켠다 |
| Expose the card to the USB host at boot | 켬 | 부팅 직후 PC가 카드를 쓸 수 있다 |
| MSC throughput report period | 1000 ms | `[msc]` 줄 출력 주기 |
| Idle time that closes a transfer session | 2000 ms | 이 시간 동안 전송이 없으면 세션 요약을 출력한다 |

SD 버스의 이론 상한은 4비트 × 클럭 ÷ 8이다.

| 모드 | 클럭 | 버스 상한 |
|---|---|---|
| Default Speed | 20 MHz | 10 MB/s |
| High Speed | 40 MHz | 20 MB/s |
| High Speed (50 MHz) | 50 MHz | 25 MB/s |
| UHS-I SDR50 | 100 MHz | 50 MB/s |

UHS-I SDR50은 카드에 1.8 V 신호를 요청(S18R)하고 CMD11로 전환한다.
전환 뒤에는 칩 내부 LDO가 SD I/O 전원을 1.8 V로 공급한다.
카드는 전원이 꺼질 때까지 1.8 V 신호를 유지한다. 초기화가 실패하면 보드 전원을 껐다 켠다.
IDF v6.1의 ESP32-S31 SDMMC 드라이버는 DDR50과 SDR104를 지원하지 않는다.

## exFAT 지원

IDF v6.1 fatfs는 `src/ffconf.h`에서 `FF_FS_EXFAT`를 0으로 고정해 두었고, menuconfig 항목도 없다.
그래서 IDF v6.1의 fatfs 컴포넌트를 `components/fatfs`로 복사했다.
프로젝트 `components` 폴더의 컴포넌트가 IDF 컴포넌트보다 우선하므로 빌드에는 이 복사본이 쓰인다.

IDF 원본과 다른 곳은 `src/ffconf.h`의 두 곳이다.

- `FF_FS_EXFAT`를 1로 바꿨다.
- `FF_USE_LABEL`이 항상 0 또는 1로 정의되게 했다.
  exFAT 전용 코드가 이 매크로를 C 식에서 쓴다.
  원본은 레이블 옵션이 꺼져 있으면 매크로가 정의되지 않아 컴파일 오류가 난다.

exFAT은 긴 파일 이름이 필요하므로 `CONFIG_FATFS_LFN_HEAP=y`를 쓴다.
IDF 버전을 바꾸면 CMake가 경고를 낸다. 그때는 새 IDF의 fatfs를 다시 복사하고 위 두 곳을 같은 방식으로 고친다.

## 측정 방법

### 1. PC에서 MSC 속도 측정

부팅하면 카드는 PC 쪽에 넘어가 있다. Windows 탐색기에 이동식 디스크로 나타난다.

```powershell
python tools\msc_bench.py E:
python tools\msc_bench.py E: --size 4096 --block 4096 --runs 1
```

- 기본값은 1 GiB 파일, 1 MiB 단위 I/O, 3회 반복이다.
- Windows 파일 캐시를 거치지 않도록 `FILE_FLAG_NO_BUFFERING`으로 쓰고 읽는다. 쓰기에는 `WRITE_THROUGH`도 붙인다.
- 512바이트마다 파일 오프셋을 새겨 넣고, 읽은 뒤 데이터를 비교한다. `--no-verify`로 끌 수 있다.
- 이동식 드라이브가 아니면 실행하지 않는다. 드라이브 문자를 잘못 넣어 다른 디스크에 쓰는 일을 막는다.
- 표준 라이브러리만 쓰므로 ESP-IDF의 파이썬(`C:\Espressif\tools\python\python.exe`)으로도 실행된다.
- 속도 단위 MB/s는 1 MB = 10^6 B 기준이다. CrystalDiskMark와 같다.

CrystalDiskMark를 쓸 때는 SEQ1M Q1T1 결과를 본다.
MSC(Bulk-Only Transport)는 명령을 한 번에 하나씩 처리하므로 큐 깊이를 늘려도 값이 거의 같다.

### 2. 장치 로그로 보기

PC가 데이터를 주고받는 동안 콘솔에 1초마다 다음 형식의 줄이 나온다.

```
[msc] RD  xx.xx MB/s (SD busy  xx%) | WR  xx.xx MB/s (SD busy  xx%)
[msc] READ  session: xxxx.x MB in xx.xx s -> xx.xx MB/s
```

- `RD`, `WR`: 장치가 USB로 보내거나 받은 데이터 양을 1초로 나눈 값이다.
- `SD busy`: 그 1초 중 `sdmmc_read_sectors()`, `sdmmc_write_sectors()` 안에 머문 시간의 비율이다.
  100%에 가까우면 SD 카드나 SD 버스가 병목이다. 낮으면 USB 쪽이나 처리 대기가 병목이다.
- `session`: 한 방향 전송이 2초 이상 멈추면 그 구간의 총량과 평균 속도를 출력한다. 1 MB 미만 구간은 출력하지 않는다.

`stats off`로 1초 단위 줄을 끈다. 세션 요약은 계속 나온다.

### 3. 장치 단독 측정

1. PC에서 드라이브를 "꺼내기"한다.
   PC가 캐시에 쓰지 않은 데이터를 남긴 채 카드를 가져오면 파일시스템이 깨질 수 있다.
2. 콘솔에서 `mount`를 입력한다. PC에서는 카드가 빠진 것처럼 보인다.
3. `rawread`, `filebench`를 실행한다.
4. `expose`로 카드를 PC에 돌려준다. PC가 카드를 다시 인식한다.

```
s31> mount
s31> rawread
s31> filebench -s 1024
s31> stats
s31> expose
```

- `rawread`는 SD 명령 크기를 4 KiB에서 1 MiB까지 바꿔 가며 원시 순차 읽기 속도를 잰다. 읽기만 하므로 데이터는 바뀌지 않는다.
- `filebench -s 1024`는 1 GiB 파일을 쓰고, 다시 읽어 검증한 뒤 지운다.
- `stats`는 SD 명령 수, 평균 명령 크기, SD 명령 시간만으로 나눈 SD 속도("while busy")를 보여 준다.

## 콘솔 명령

| 명령 | 설명 |
|---|---|
| `info` | 카드 레지스터(CID/CSD/SCR/SSR), 버스 모드와 클럭, USB 연결 속도, 카드 소유자, 파일시스템 |
| `mount` | PC에서 카드를 가져와 `/sdcard`에 마운트한다. PC에서 먼저 꺼내기를 한다 |
| `expose` | `/sdcard`를 언마운트하고 카드를 PC에 넘긴다 |
| `stats [on\|off\|reset]` | USB·SD 누적 통계, 1초 단위 출력 켜기·끄기, 카운터 초기화 |
| `rawread [-s MiB] [-c KiB] [-o MiB]` | 원시 순차 읽기. 기본은 카드 중간부터 64 MiB 또는 5초씩, 명령 크기 4 KiB~1 MiB |
| `filebench [-s MiB] [-b KiB] [-n] [-k]` | `/sdcard/s31bench.bin` 쓰기·읽기·검증. 기본 256 MiB, 호출당 256 KiB. `-n` 검증 생략, `-k` 파일 유지 |
| `format [-t exfat\|fat32] [-c KiB] --yes` | 카드 전체를 포맷한다(MBR, 파티션 1개, 데이터 영역 4 MiB 정렬). 기본 exFAT, 클러스터 128 KiB |
| `restart` | 칩을 재시작한다 |

`format`은 카드의 모든 데이터를 지운다. `mount` 뒤에만 실행되고 `--yes`가 있어야 한다.

## 결과 해석

PC 측 값이 SD 버스 상한보다 많이 낮으면 다음을 확인한다.

- esp_tinyusb의 MSC 읽기는 SD에서 32 KiB를 읽어 USB로 보낸 뒤에 다음 32 KiB를 읽는다.
  SD 읽기와 USB 전송이 겹치지 않으므로 MSC 읽기 속도는 대략 1 / (1/SD 속도 + 1/USB 속도)가 된다.
- 쓰기는 USB 수신과 이전 블록의 SD 쓰기가 일부 겹친다. esp_tinyusb가 SD 쓰기를 TinyUSB 태스크로 미뤄 처리하기 때문이다.
- PC가 보내는 큰 READ(10)/WRITE(10)도 장치 안에서는 32 KiB 단위로 나뉜다.
  ESP32-S31의 MSC 버퍼 상한이 32 KiB(`CONFIG_TINYUSB_MSC_BUFSIZE`)이기 때문이다.
  그래서 `rawread`의 32 KiB 행이 MSC 경로의 SD 성능에 가장 가깝다. `stats`의 평균 명령 크기로 확인할 수 있다.
- 파일시스템 정렬도 쓰기 속도에 영향을 준다.
  `info`가 데이터 영역이 AU 경계에 있지 않다고 표시하면 카드를 다시 포맷한다.
  SD Association의 SD Memory Card Formatter나 `format --yes`를 쓴다.

## 구현 메모

- **카드 소유권**: esp_tinyusb의 자동 전환(USB 연결 시 PC, 분리 시 앱)을 껐다. `mount`와 `expose`로만 바뀐다.
- **자동 포맷 금지**: 마운트에 실패해도 카드를 포맷하지 않는다(`do_not_format`).
- **PC 접근 차단**: 앱이 카드를 가진 동안 PC의 TEST UNIT READY, READ(10), WRITE(10)에 "매체 없음"으로 응답한다.
  카드를 돌려줄 때는 "매체가 바뀌었을 수 있음"(UNIT ATTENTION)을 한 번 보내 PC가 카드를 다시 읽게 한다.
- **링커 래퍼**: `main/CMakeLists.txt`에서 `-Wl,--wrap`으로 다섯 함수를 감싼다.
  - `tud_msc_test_unit_ready_cb`, `tud_msc_read10_cb`, `tud_msc_write10_cb`: USB 바이트 수 집계, PC 접근 차단
  - `sdmmc_read_sectors`, `sdmmc_write_sectors`: SD 점유 시간 측정, 비정렬 버퍼 복사
- **비정렬 버퍼 복사**: esp_tinyusb의 쓰기 버퍼는 구조체 안 28바이트 위치에 있어 캐시 라인 정렬이 보장되지 않는다.
  SDMMC 드라이버는 정렬되지 않은 버퍼를 8 KiB씩 나눠 보낸다.
  그래서 32 KiB 정렬 버퍼에 한 번 복사해 SD 명령 하나로 보낸다. 복사 횟수는 `stats`의 bounce 항목에 나온다.
- **용량**: READ(10)/WRITE(10)의 32비트 LBA를 쓰므로 2 TiB 카드까지 다룰 수 있다.

## 확인한 범위

확인한 것:

- ESP-IDF v6.1(`C:\esp\v6.1\esp-idf`)과 esp_tinyusb 2.2.0, tinyusb 0.19.0~3으로 빌드했다. 오류와 경고는 없다.
  펌웨어 크기는 약 420 KiB이다.
- ELF에서 래퍼 연결을 확인했다.
  TinyUSB MSC 클래스는 `__wrap_tud_msc_*`를 호출하고, esp_tinyusb와 FatFs의 SD 접근은 `__wrap_sdmmc_*`를 거친다.
- exFAT 코드(`load_xdir`, `change_bitmap` 등)가 링크된 것을 확인했다.
- `tools/msc_bench.py`를 PC의 로컬 NTFS 드라이브에서 실행했다.
  쓰기, 읽기, 검증, 테스트 파일 삭제가 동작했다.

보드에서 아직 확인하지 않은 것:

- 보드에서 실행해 보지 않았다. 카드 인식, USB 열거, 측정값 모두 확인이 필요하다.
- Type-A 포트를 PC에 연결할 때의 VBUS 처리(위 주의 참고)
- High Speed 50 MHz와 UHS-I SDR50 동작
- GPIO39(SD_CTRL)의 극성
- PSRAM 설정(Octal, 200 MHz)은 같은 모듈을 쓰는 ESP32S31-CANFD 프로젝트 설정을 따랐다.
