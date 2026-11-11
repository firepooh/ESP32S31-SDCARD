# ESP32-S31-Korvo-1 microSD USB High-Speed MSC 속도 측정

> **상태**: 보드에서 SD 카드 동작(High Speed 40 MHz, UHS-I SDR50 100 MHz)과 512 GB exFAT 마운트를 확인했다.
> PC와 USB High-Speed로 연결한 MSC 속도는 쓰기 21.3 MB/s, 읽기 20.2 MB/s다(SDR50, 미리 읽기·쓰기 모으기 적용).
> 측정값과 병목 분석은 [벤치 결과](#벤치-결과)에 있다.

## 사용 보드

| 항목 | 내용 |
|---|---|
| 보드 | Espressif [ESP32-S31-Korvo-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/index.html) V1.1 |
| 사용자 가이드 | [ESP32-S31-Korvo-1 V1.1 User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/user_guide.html) |
| 모듈 | ESP32-S31-WROOM-3 (플래시 16 MB, PSRAM 16 MB) |
| 사용 기능 | microSD 슬롯(4비트 SDMMC), USB 2.0 Type-A 포트(High-Speed OTG), USB-to-UART 콘솔 |
| SD 카드 | 64 GB 이상 microSDXC, exFAT (FAT32 카드도 쓸 수 있다) |
| 개발 환경 | ESP-IDF v6.1 (Windows), esp_tinyusb 2.2.0 |

![ESP32-S31-Korvo-1 V1.1 보드 구성](docs/images/esp32-s31-korvo-1.webp)

이 프로젝트에서 쓰는 부분은 사진 번호로 다음과 같다.

| 번호 | 부분 | 용도 |
|---|---|---|
| (2) | USB Type-C 포트 (UART) | 전원, 플래시, 콘솔 (COM 포트) |
| (4) | 전원 스위치 | 보드 5 V 전원 |
| (5) | USB 2.0 Type-A 포트 | USB High-Speed MSC (PC 연결) |
| (22) | ESP32-S31-WROOM-3 | MCU 모듈 |
| (23) | microSD 카드 슬롯 | 측정 대상 카드 (SDMMC 슬롯 0, UHS-I) |

사진 출처: Espressif [ESP32-S31-Korvo-1 V1.1 사용자 가이드](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/user_guide.html)

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

## 벤치 결과

2026-11-10에 보드 단독으로 측정했다(USB 미연결). 속도 단위 MB/s는 1 MB = 10^6 B 기준이다.

| 항목 | 내용 |
|---|---|
| 보드 | ESP32-S31-Korvo-1 V1.1, CPU 320 MHz |
| 카드 | 삼성 microSDXC 512 GB (CID PNM "EF8S5", SD 3.0, UHS-I, AU 4 MB) |
| 파일시스템 | exFAT, 클러스터 256 KiB, 데이터 영역 4 MB 정렬 |
| SD 버스 | 4비트. High Speed 40 MHz(3.3 V 신호)와 UHS-I SDR50 100 MHz(1.8 V 신호)를 비교했다 |
| 펌웨어 | 이 저장소, ESP-IDF v6.1 |

### 원시 순차 읽기 (`rawread`)

카드 중간 위치에서 SD 명령 크기별로 64 MiB 또는 5초씩 읽었다. 파일시스템을 거치지 않는다.

| SD 명령 크기 | High Speed 40 MHz (MB/s) | UHS-I SDR50 100 MHz (MB/s) | SDR50 / HS |
|---|---|---|---|
| 4 KiB | 10.68 | 13.69 | 1.3배 |
| 16 KiB | 15.98 | 29.54 | 1.8배 |
| 32 KiB | 17.54 | 36.27 | 2.1배 |
| 64 KiB | 18.44 | 41.54 | 2.3배 |
| 128 KiB | 18.92 | 44.87 | 2.4배 |
| 256 KiB | 19.18 | 46.69 | 2.4배 |
| 512 KiB | 19.30 | 47.61 | 2.5배 |
| 1024 KiB | **19.37** | **48.08** | 2.5배 |
| 버스 이론 상한 | 20 | 50 | 2.5배 |

- 명령이 크면 두 모드 모두 버스 이론 상한의 96~97 %까지 나온다.
- 명령이 작으면 클럭을 올려도 덜 빨라진다. 명령마다 명령·응답 교환과 카드 준비에 드는 고정 시간이 있기 때문이다.
  4 KiB 명령 한 번에 High Speed는 384 µs, SDR50은 299 µs가 걸렸다.
- USB MSC 경로는 SD를 32 KiB 단위로 읽고 쓴다. 그래서 32 KiB 행이 USB 연결 때 SD 쪽 상한에 가깝다.

### exFAT 파일 쓰기·읽기 (`filebench -s 1024`)

1 GiB 파일을 256 KiB씩 쓰고 다시 읽은 뒤, 512바이트마다 데이터를 검증했다.

| 모드 | 쓰기 (MB/s) | 읽기 (MB/s) | 데이터 검증 |
|---|---|---|---|
| High Speed 40 MHz | 14.48 | 18.91 | OK |
| UHS-I SDR50 100 MHz | **29.15** | **45.58** | OK |

- 쓰기는 클러스터(256 KiB)마다 SD 명령 하나로 나간다. 평균 명령 크기는 255.6 KiB였다.
- 256 KiB 쓰기 한 번은 버스 전송 시간에 카드 내부 기록 시간이 더해진다.
  - SDR50: 약 9.0 ms = 버스 전송 5.2 ms + 카드 기록 약 3.7 ms
  - High Speed 40 MHz: 약 18.1 ms = 버스 전송 13.1 ms + 카드 기록 약 5.0 ms
- 그래서 쓰기는 버스 상한보다 낮고, 카드 자체 성능의 영향을 크게 받는다.

SDR50에서 읽기 명령 29,605회(1,611 MB)와 쓰기 명령 4,103회(1,074 MB)를 실행했다. SD 오류는 없었다.

### USB MSC 속도 (PC에서 측정)

SDR50 설정에서 USB 2.0 Type-A 포트를 PC에 연결하고 `python tools\msc_bench.py D:`로 측정했다.

- USB는 High-Speed(480 Mbit/s)로 연결됐고, Windows에 exFAT 이동식 디스크(D:)로 나타났다.
- 조건: 1 GiB 파일, 1 MiB 단위 I/O, Windows 캐시 미사용, 3회 반복, 데이터 검증

두 가지 MSC 경로를 비교했다. 각각 1 GiB를 3회씩 쓰고 읽었고, 모든 회차에서 데이터 검증을 통과했다.

| MSC 경로 | PC 쓰기 (MB/s) | PC 읽기 (MB/s) |
|---|---|---|
| esp_tinyusb 기본 경로 (`BENCH_MSC_FAST_IO=n`) | 9.01 | 13.65 |
| **MSC 엔진: 미리 읽기 + 쓰기 모으기 (기본값)** | **21.29** | **20.23** |
| 배율 | 2.4배 | 1.5배 |

회차별 값 (장치 값은 장치 로그의 `[msc] ... session` 요약으로, USB로 오간 데이터 양을 구간 시간으로 나눈 값이다):

| MSC 경로 | 회차 | PC 쓰기 | PC 읽기 | 장치 쓰기 | 장치 읽기 | 검증 |
|---|---|---|---|---|---|---|
| 기본 경로 | 1 | 9.13 | 13.56 | 9.09 | 13.26 | OK |
| 기본 경로 | 2 | 8.90 | 13.71 | 8.87 | 13.40 | OK |
| 기본 경로 | 3 | 9.01 | 13.69 | 8.99 | 13.42 | OK |
| MSC 엔진 | 1 | 21.38 | 20.22 | 21.19 | 19.42 | OK |
| MSC 엔진 | 2 | 21.23 | 20.23 | 20.99 | 19.43 | OK |
| MSC 엔진 | 3 | 21.26 | 20.23 | 20.99 | 19.45 | OK |

측정 구간의 SD 통계(`stats`):

| 항목 | 기본 경로 읽기 | 기본 경로 쓰기 | 엔진 읽기 | 엔진 쓰기 |
|---|---|---|---|---|
| SD 명령당 평균 크기 | 32 KiB | 32 KiB | 244.9 KiB | 508.4 KiB |
| SD busy (전체 시간 중 SD 명령 시간) | 41 % | 79 % | 42 % | 60 % |
| SD 명령 시간만으로 계산한 속도 | 32.5 MB/s | 11.4 MB/s | 45.8 MB/s | 34.9 MB/s |
| SD 오류 | 0 | 0 | 0 | 0 |

엔진 카운터: 미리 읽기 적중 98,368회, 실패 77회, 대기 8회, 버린 미리 읽기 0회.
쓰기 모으기는 SD 명령 6,188회, 주소 불연속으로 끊긴 경우 39회, 버퍼가 빌 때까지 PC가 기다린 경우 42회였다.

**기본 경로에서 막히던 이유** (32 KiB 한 덩어리 기준)

- 읽기 2.40 ms: SD 읽기 1.01 ms와 나머지 1.39 ms(USB 전송, TinyUSB, PC 명령 처리)가 차례로 일어난다.
  esp_tinyusb는 SD에서 32 KiB를 읽어 USB로 보낸 뒤에야 다음 32 KiB를 읽는다. 두 시간이 겹치지 않고 더해진다.
- 쓰기 3.64 ms: SD 쓰기 한 번에 2.87 ms가 걸린다. 버스 전송 0.66 ms에 카드 내부 기록 시간 약 2.2 ms가 더해진다.
  32 KiB마다 SD 명령을 따로 보내므로 이 고정 시간이 매번 붙는다.

**MSC 엔진이 바꾼 것** ([구현 메모](#구현-메모) 참고)

- 읽기: 별도 태스크가 256 KiB 단위로 미리 읽는다. USB 콜백은 메모리 복사만 하므로 SD 읽기와 USB 전송이 겹친다.
- 쓰기: 주소가 이어지는 덩어리를 512 KiB까지 모아 SD 명령 하나로 쓴다. PC는 그동안 다른 버퍼에 계속 보낸다.

**엔진 적용 뒤 남은 병목**

- SD는 더 이상 병목이 아니다. 읽기 때 42 %, 쓰기 때 60 % 시간만 바쁘다.
- 32 KiB 한 덩어리에 읽기 1.62 ms, 쓰기 1.54 ms가 걸린다. 대부분 USB 쪽 시간이다.
- 그중 약 0.3 ms는 PSRAM 버퍼와 TinyUSB 버퍼(내부 RAM) 사이의 memcpy다. 부팅 로그에서 297 µs와 287 µs로 측정됐다.
- 나머지 약 1.2~1.3 ms는 USB 전송, TinyUSB 처리, PC의 명령 처리 시간이다.
- 더 올리려면 memcpy를 줄이거나(내부 RAM 버퍼), 한 번에 옮기는 덩어리를 키워야 한다(TinyUSB 상한은 64 KiB 미만).

## UHS-I SDR50 동작

### 요약

- 이번 카드는 UHS-I를 지원한다. 1.8 V 전환 요청에 S18A=1로 응답했다.
- ESP32-S31의 SDMMC 슬롯 0은 SD 3.0(SDIO 3.0)의 UHS-I를 지원한다.
- 1.8 V 전환은 칩 안에서 자동으로 이뤄진다.
  CMD11 전환 때 IDF 드라이버가 SD I/O 패드 전원을 칩 내부 LDO(1.8 V)로 바꾼다.
- Korvo-1에는 SD용 외부 LDO나 레벨 시프터가 없다.
  SD 신호선이 S31 슬롯 0 핀(GPIO20~25)에 바로 연결돼 있다(회로도 "SDIO 3.0 Host" 블록).
- 세 조건이 모두 맞아 SDR50 100 MHz로 동작했다.

### SDR50의 뜻과 속도 계산

SDR50은 UHS-I의 "Single Data Rate, 50 MB/s" 모드다.

- 클럭은 100 MHz이고, 클럭 상승 에지마다 데이터선 4개(DAT0~3)로 4비트를 보낸다.
- 버스 속도 = 100 MHz × 4비트 ÷ 8 = **50 MB/s**. 비트 하나의 시간은 10 ns다.
- 이름의 50은 클럭(MHz)이 아니라 버스 속도(MB/s)다.
- 측정한 원시 읽기 48.08 MB/s는 이 상한의 96 %다.

| 모드 | 신호 전압 | 클럭 | 샘플링 | 버스 상한 | ESP32-S31 (IDF v6.1) |
|---|---|---|---|---|---|
| Default Speed | 3.3 V | 25 MHz (IDF는 20 MHz) | 상승 에지 | 12.5 MB/s | 지원 |
| High Speed | 3.3 V | 50 MHz (IDF 기본 40 MHz) | 상승 에지 | 25 MB/s | 지원 |
| UHS-I SDR50 | 1.8 V | 100 MHz | 상승 에지 | 50 MB/s | 지원, 이 보드에서 확인 |
| UHS-I DDR50 | 1.8 V | 50 MHz | 상승·하강 에지 | 50 MB/s | 드라이버 미지원 |
| UHS-I SDR104 | 1.8 V | 208 MHz | 상승 에지 | 104 MB/s | 드라이버 미지원 |

DDR50은 클럭을 50 MHz로 낮추고 양쪽 에지에서 샘플링해 같은 50 MB/s를 낸다.

### 1.8 V 전환 순서

1. 카드 전원을 켠다. 3.3 V 신호, 400 kHz 클럭으로 시작한다.
2. CMD0으로 카드를 초기화하고, CMD8로 동작 전압을 확인한다.
3. ACMD41에서 호스트가 S18R 비트로 1.8 V를 요청하고, 카드가 S18A=1로 수락한다.
4. CMD11로 카드와 S31이 신호를 1.8 V로 바꾼다. 이때 S31은 SD I/O 패드 전원을 내부 LDO로 바꾼다.
5. CMD6로 SDR50 기능을 고른다.
6. 호스트 클럭을 100 MHz로 올린다.
7. CMD19 튜닝으로 데이터 샘플링 시점을 고른다. 이 보드에서는 지연 단계 2가 선택됐다.

카드 전원(VDD)은 UHS-I에서도 3.3 V(SD_3V3)다. 1.8 V로 바뀌는 것은 CLK, CMD, DAT0~3 신호선뿐이다.

### 카드 전원 재인가 (GPIO39)

- 카드는 전원이 새로 들어온 직후에만 1.8 V 전환을 수락한다. 한 번 1.8 V로 바뀌면 전원이 꺼질 때까지 그대로 유지한다.
- 칩 리셋이나 플래시 후 리셋으로는 카드 전원이 끊기지 않는다. 첫 시험에서 카드가 S18A=0으로 거절한 이유다.
- Korvo-1은 SD_CTRL(GPIO39)로 카드 전원 스위치 Q1(AO3401A, P-MOSFET)을 제어한다. LOW = 켜짐, HIGH = 꺼짐이다.
  회로는 [microSD 회로](#microsd-회로)에 있다.
- 펌웨어는 부팅 때마다 GPIO39로 카드 전원을 1초 끊는다.
- 끊는 동안 SD 신호선을 LOW로 둔다. 신호선으로 카드에 전원이 새어 들어가지 않게 하고, 방전 저항이 없는 SD_3V3의 10 µF를 비우기 위해서다.

### 카드가 전환을 거절할 때

- IDF 드라이버는 카드가 3.3 V High Speed에 머물러도 요청한 100 MHz를 그대로 적용한다. High Speed 규격 상한(50 MHz)의 2배다.
- 펌웨어는 이 경우를 감지해 High Speed 40 MHz로 내려가고 경고 로그를 남긴다.

### SDR50 정상 진입 부팅 로그

```
I sd: card power cycled with GPIO39 (off 1000 ms, on level 0)
I sd: slot 0, 4-bit, requested mode: UHS-I SDR50 100 MHz
I sdmmc_sd: sdmmc_do_timing_tuning: proper delay phase/line id: 2
Type: SDHC/SDXC (UHS-I)
Bus: 4-bit, UHS-I SDR50 100 MHz, UHS-I active, clock 100000 kHz (card limit 100000 kHz)
OCR: 0xc1ff8000 (CCS 1, S18A 1) | host sent S18R: yes
```

### 설정과 확인 범위

- menuconfig `S31 SD MSC benchmark`에서 `SD bus speed mode`를 `UHS-I SDR50`으로 고른다.
  `Power-cycle the card with a GPIO`는 켜 둔다(기본값).
- 프로토콜 수준에서 확인했다. S18A=1, CMD11 전환, 튜닝 통과, 100 MHz에서 약 2.7 GB를 오류 없이 주고받았다.
- 신호선의 실제 전압(약 1.8 V 진폭)은 오실로스코프로 재지 않았다.

## 보드 연결

| 항목 | 내용 |
|---|---|
| microSD | SDMMC 슬롯 0, 4비트. CLK GPIO24, CMD GPIO25, D0~D3 GPIO20~23 (IO MUX 고정 핀) |
| SD_CTRL | GPIO39. 카드 전원 스위치 Q1(AO3401A, P-MOSFET)의 게이트. LOW = 켜짐, HIGH = 꺼짐 (R138 풀다운으로 부팅 때 켜짐) |
| USB MSC | USB 2.0 Type-A 포트 (ESP32-S31 High-Speed OTG, UTMI PHY) |
| 콘솔 | USB-to-UART Type-C 포트 (UART0, GPIO58/59), 115200 bps |

### microSD 회로

![ESP32-S31-Korvo-1 V1.1 microSD 회로 (SDIO 3.0 Host)](docs/images/esp32-s31-korvo-1-sd-circuit.png)

회로도 출처: Espressif ESP32-S31-Korvo-1 V1.1 회로도 ("SDIO 3.0 Host" 블록)

| 부분 | 회로 | 펌웨어와의 관계 |
|---|---|---|
| 신호선 | GPIO20~25가 0 Ω 저항(R7, R65~R69)만 거쳐 카드의 D0~D3, CLK, CMD에 연결된다. 레벨 시프터가 없다 | 신호 전압은 S31 패드 전원이 정한다. UHS-I 때 칩 내부 LDO가 1.8 V로 바꾼다 |
| SPI NAND 겸용 | 같은 GPIO가 R22, R23, R1~R4(NC)를 거쳐 NAND 플래시 자리로도 갈 수 있다 | 기본 조립 상태에서는 NC라 microSD만 연결된다 |
| 풀업 | 신호선에 외부 풀업 저항이 없다 | 칩 내부 풀업을 켠다(`Enable internal pull-ups`). 3.3 V 외부 풀업이 없어서 1.8 V 신호와 충돌하지 않는다 |
| 카드 전원 | VCC_3V3이 Q1(AO3401A, P-MOSFET)을 거쳐 SD_3V3(카드 VDD)이 된다 | UHS-I에 필요한 카드 전원 재인가에 쓴다 |
| SD_CTRL | Q1 게이트 = GPIO39. R138 10 kΩ 풀다운, R70(풀업)은 NC | LOW = 켜짐, HIGH = 꺼짐. 부팅 때 펌웨어가 1초 동안 HIGH로 둔다 |
| 디커플링 | SD_3V3에 C1 10 µF, C16 0.1 µF. 방전 저항은 없다 | 전원을 끄는 동안 신호선을 LOW로 두어 카드 쪽으로 전원이 새지 않게 한다 |
| ESD | 모든 신호선과 SD_DET에 BSDFN2C031U | - |
| 카드 감지 | 카드 CD 핀이 SD_DET로 나와 TP5에 연결된다 | 이 회로도 범위에서는 GPIO에 연결되지 않아 펌웨어는 카드 감지를 쓰지 않는다 |
| 배선 | 단일 종단 50 Ω, 길이 맞춤(3 mil 이내), 3 cm 이하 | 100 MHz(SDR50) 동작의 전제 조건이다 |

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
  msc_io.c                MSC 엔진: 미리 읽기, 쓰기 모으기
  bench.c                 rawread, filebench, format, 파일시스템 정보
  cmd_bench.c             콘솔 명령 등록
  Kconfig.projbuild       menuconfig "S31 SD MSC benchmark"
  idf_component.yml       espressif/esp_tinyusb ^2.2.0
components/fatfs/         ESP-IDF v6.1 fatfs 복사본 (exFAT 켬)
docs/images/              보드 사진, microSD 회로도
tools/msc_bench.py        PC 측 순차 쓰기·읽기 측정 (Windows)
dependencies.lock         컴포넌트 버전 고정 (esp_tinyusb 2.2.0, tinyusb 0.19.0~3)
.github/workflows/
  release.yml             v* 태그 → IDF v6.1 빌드 → GitHub 릴리즈
  pages.yml               릴리즈 바이너리를 모아 웹 플래셔를 GitHub Pages에 배포
.github/release-notes.md  릴리즈 본문 템플릿
web/index.html            웹 플래셔 (esptool-js)
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

## 릴리즈와 웹 플래셔

`v`로 시작하는 태그를 푸시하면 GitHub Actions가 펌웨어를 빌드해 릴리즈에 올린다.
그 뒤 웹 플래셔 페이지도 다시 만들어 새 버전을 목록에 넣는다.

```bash
git tag v1.0.0
git push origin v1.0.0
```

| 단계 | 워크플로 | 내용 |
|---|---|---|
| 빌드 | `release.yml` | `espressif/idf:v6.1` Docker 이미지에서 `idf.py --preview build`와 `merge-bin`을 실행한다 |
| 릴리즈 | `release.yml` | `*-merged.bin`, `*-all.zip`, `SHA256SUMS.txt`를 태그 릴리즈에 올린다 |
| 웹 플래셔 | `pages.yml` | 모든 릴리즈의 merged.bin과 `manifest.json`을 모아 GitHub Pages에 배포한다 |

- 앱 버전은 태그 이름이다. CI가 `version.txt`를 만들어 IDF에 넘긴다.
- `v1.1.0-rc1`처럼 `-`가 들어간 태그는 프리릴리즈로 올린다.
- GitHub 웹에서 릴리즈를 먼저 만들어도 된다. 이때는 릴리즈 본문은 그대로 두고 파일만 추가한다.
- Actions 탭에서 `Release build`를 수동 실행하면 릴리즈 없이 빌드 결과만 아티팩트로 남긴다.
- CI도 로컬과 같은 컴포넌트 버전으로 빌드하도록 `dependencies.lock`을 저장소에 넣었다.
  MSC 엔진은 esp_tinyusb 내부 함수를 링커 래퍼로 가로채므로 버전이 바뀌면 동작을 보장할 수 없다.

### 웹 플래셔

주소: <https://firepooh.github.io/ESP32S31-SDCARD/>

1. 데스크톱 Chrome이나 Edge에서 연다. Web Serial을 쓰므로 다른 브라우저는 안 된다.
2. Korvo-1의 Type-C UART 포트를 PC에 연결한다.
3. 버전을 고르고 **연결**과 **플래시**를 차례로 누른다. 칩이 ESP32-S31이 아니면 굽지 않는다.

- 플래시 엔진은 esptool-js 0.7.1이다. ESP32-S31은 0.7.0부터 지원한다.
- 기본값은 스텁 없이 ROM 로더로 굽는 것이다. IDF v6.1도 ESP32-S31을 `--no-stub`으로 굽는다.
- 펌웨어 파일은 페이지와 같은 도메인(`firmware/<태그>/`)에 둔다.
  브라우저 스크립트는 CORS 제한 때문에 GitHub 릴리즈 파일을 직접 읽을 수 없다.
- 페이지는 내려받은 파일의 SHA-256을 `manifest.json` 값과 비교한 뒤 굽는다.

처음 한 번은 저장소 설정에서 Pages를 켜야 한다. **Settings > Pages > Source**를 **GitHub Actions**로 바꾼다.

확인한 범위:

- 로컬 IDF v6.1에서 CI와 같은 빌드, `merge-bin`, 패키징 명령을 실행해 결과 파일을 확인했다.
- 웹 플래셔를 로컬 서버로 띄워 목록 표시, 파일 내려받기, esptool-js 로드를 확인했다.
- GitHub Actions 실행, Pages 배포, 웹 플래셔로 실제 보드에 굽는 것은 아직 확인하지 않았다.

## menuconfig 설정

`idf.py --preview menuconfig`의 `S31 SD MSC benchmark` 메뉴에 있다.

| 항목 | 기본값 | 설명 |
|---|---|---|
| SD bus speed mode | **UHS-I SDR50 100 MHz** | Default Speed 20 MHz, High Speed 40 MHz, High Speed 50 MHz(실험), UHS-I SDR50 100 MHz(보드에서 확인). 카드가 UHS-I를 거절하면 High Speed 40 MHz로 내려간다 |
| SD bus width | 4 lines | 1비트로 바꿀 수 있다 |
| Enable internal pull-ups | 켬 | 보드 풀업에 내부 풀업을 더한다 (IDF 예제와 같다) |
| On-chip LDO channel | 1 | SDR50에서만 쓴다. IDF sd_card 예제의 ESP32-S31 값과 같다 |
| Power-cycle the card with a GPIO | 켬 | 부팅 때 GPIO39로 카드 전원을 1초 끊었다가 켠다. UHS-I에 필요하다 |
| Expose the card to the USB host at boot | 켬 | 부팅 직후 PC가 카드를 쓸 수 있다 |
| MSC throughput report period | 1000 ms | `[msc]` 줄 출력 주기 |
| Idle time that closes a transfer session | 2000 ms | 이 시간 동안 전송이 없으면 세션 요약을 출력한다 |
| MSC read-ahead and write merging | 켬 | MSC 엔진. 끄면 esp_tinyusb 기본 경로로 동작한다 |
| Read-ahead buffer size | 256 KiB | 미리 읽기 버퍼 하나의 크기. 버퍼는 두 개다 |
| Put the read-ahead buffers in internal RAM | 끔 | 켜면 memcpy가 빨라지지만 내부 RAM 여유가 약 300 KiB뿐이다 |
| Write merge buffer size | 512 KiB | 쓰기 모으기 버퍼 하나의 크기. 버퍼는 두 개이고 PSRAM에 둔다 |
| Write back merged data after this host idle time | 20 ms | PC 쓰기가 이 시간 동안 없으면 모아 둔 데이터를 카드에 쓴다 |

기본값은 `sdkconfig.defaults`에 있다.
모드별 클럭과 버스 상한, 1.8 V 전환 과정은 [UHS-I SDR50 동작](#uhs-i-sdr50-동작)에 정리했다.

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
이 보드에서 측정한 수치와 시간 분해는 [USB MSC 속도](#usb-msc-속도-pc에서-측정)에 있다.
아래 두 항목은 MSC 엔진을 끈 기본 경로의 동작이다. 엔진을 켜면 SD 접근이 USB 전송과 겹치고 SD 명령이 커진다.

- esp_tinyusb의 MSC 읽기는 SD에서 32 KiB를 읽어 USB로 보낸 뒤에 다음 32 KiB를 읽는다.
  SD 읽기와 USB 전송이 겹치지 않으므로 MSC 읽기 속도는 대략 1 / (1/SD 속도 + 1/USB 속도)가 된다.
- 쓰기는 USB 수신과 이전 블록의 SD 쓰기가 일부 겹친다. esp_tinyusb가 SD 쓰기를 TinyUSB 태스크로 미뤄 처리하기 때문이다.
  대신 SD 쓰기가 32 KiB 단위라 명령마다 카드 기록 고정 시간이 붙는다.
- PC가 보내는 큰 READ(10)/WRITE(10)도 장치 안에서는 32 KiB 단위로 나뉜다.
  ESP32-S31의 MSC 버퍼 상한이 32 KiB(`CONFIG_TINYUSB_MSC_BUFSIZE`)이기 때문이다.
  그래서 `rawread`의 32 KiB 행이 MSC 경로의 SD 성능에 가장 가깝다(High Speed 17.54 MB/s, SDR50 36.27 MB/s).
  `stats`의 평균 명령 크기로 확인할 수 있다.
- 파일시스템 정렬도 쓰기 속도에 영향을 준다.
  `info`가 데이터 영역이 AU 경계에 있지 않다고 표시하면 카드를 다시 포맷한다.
  SD Association의 SD Memory Card Formatter나 `format --yes`를 쓴다.

## 구현 메모

- **카드 소유권**: esp_tinyusb의 자동 전환(USB 연결 시 PC, 분리 시 앱)을 껐다. `mount`와 `expose`로만 바뀐다.
- **자동 포맷 금지**: 마운트에 실패해도 카드를 포맷하지 않는다(`do_not_format`).
- **PC 접근 차단**: 앱이 카드를 가진 동안 PC의 TEST UNIT READY, READ(10), WRITE(10)에 "매체 없음"으로 응답한다.
  카드를 돌려줄 때는 "매체가 바뀌었을 수 있음"(UNIT ATTENTION)을 한 번 보내 PC가 카드를 다시 읽게 한다.
- **링커 래퍼**: `main/CMakeLists.txt`에서 `-Wl,--wrap`으로 여섯 함수를 감싼다.
  - `tud_msc_test_unit_ready_cb`, `tud_msc_read10_cb`, `tud_msc_write10_cb`: USB 바이트 수 집계, PC 접근 차단, MSC 엔진 연결
  - `tud_msc_scsi_cb`: SYNCHRONIZE CACHE와 꺼내기 때 모아 둔 쓰기 데이터를 카드에 쓴다
  - `sdmmc_read_sectors`, `sdmmc_write_sectors`: SD 점유 시간 측정, 비정렬 버퍼 복사
- **비정렬 버퍼 복사**: esp_tinyusb의 쓰기 버퍼는 구조체 안 28바이트 위치에 있어 캐시 라인 정렬이 보장되지 않는다.
  SDMMC 드라이버는 정렬되지 않은 버퍼를 8 KiB씩 나눠 보낸다.
  그래서 32 KiB 정렬 버퍼에 한 번 복사해 SD 명령 하나로 보낸다. 복사 횟수는 `stats`의 bounce 항목에 나온다.
  ESP32-S31에서는 USB MSC 측정 중 복사가 한 번도 일어나지 않았다. 쓰기 버퍼가 드라이버의 정렬 검사를 통과했다.
  다른 칩이나 라이브러리 버전에서 정렬 조건이 달라질 때를 대비해 남겨 두었다.
- **용량**: READ(10)/WRITE(10)의 32비트 LBA를 쓰므로 2 TiB 카드까지 다룰 수 있다.

### MSC 엔진 (`main/msc_io.c`)

TinyUSB는 MSC 데이터를 32 KiB 덩어리로 옮기고 덩어리마다 READ(10)/WRITE(10) 콜백을 부른다.
엔진은 이 콜백(링커 래퍼) 안에서 esp_tinyusb 대신 카드 접근을 맡는다.

- **미리 읽기**: 버퍼 두 개(기본 256 KiB씩)를 번갈아 쓴다.
  PC가 이어지는 주소를 읽으면 `msc_io` 태스크가 다음 구간을 카드에서 미리 읽는다.
  콜백은 버퍼에서 TinyUSB 버퍼로 복사만 한다.
- **쓰기 모으기**: 버퍼 두 개(기본 512 KiB씩)를 번갈아 쓴다.
  주소가 이어지는 덩어리를 모았다가 `msc_io` 태스크가 SD 명령 하나로 쓴다. 그동안 PC는 다른 버퍼로 계속 보낸다.
- **일관성**
  - 쓰기가 들어오면 미리 읽은 데이터를 버린다. 진행 중인 미리 읽기는 세대 번호로 무효 처리한다.
  - 읽기가 들어오면 모아 둔 쓰기 데이터를 먼저 카드에 쓴다.
- **모아 둔 데이터를 카드에 쓰는 시점**
  - PC 쓰기가 20 ms 동안 없을 때
  - SYNCHRONIZE CACHE, PREVENT ALLOW MEDIUM REMOVAL(꺼내기) 명령을 받을 때
  - `mount`로 카드를 가져올 때
- **주의 (write-back)**: PC는 데이터가 카드에 기록되기 전에 완료 응답을 받는다.
  쓰기 직후 20 ms 안에 전원이 끊기면 최대 1 MiB(512 KiB × 2)를 잃을 수 있다.
  백그라운드 쓰기가 실패하면 다음 WRITE(10)나 SYNCHRONIZE CACHE에 오류로 알린다.
  안전이 더 중요하면 menuconfig에서 엔진을 끈다.

## 확인한 범위

확인한 것:

- ESP-IDF v6.1(`C:\esp\v6.1\esp-idf`)과 esp_tinyusb 2.2.0, tinyusb 0.19.0~3으로 빌드했다. 오류와 경고는 없다.
  펌웨어 크기는 약 420 KiB이다.
- ELF에서 래퍼 연결을 확인했다.
  TinyUSB MSC 클래스는 `__wrap_tud_msc_*`를 호출하고, esp_tinyusb와 FatFs의 SD 접근은 `__wrap_sdmmc_*`를 거친다.
- exFAT 코드(`load_xdir`, `change_bitmap` 등)가 링크된 것을 확인했다.
- `tools/msc_bench.py`를 PC의 로컬 NTFS 드라이브에서 실행했다.
  쓰기, 읽기, 검증, 테스트 파일 삭제가 동작했다.
- 보드에서 부팅, PSRAM(Octal 200 MHz), 512 GB 카드 인식, exFAT 진단과 마운트가 정상이었다.
- 보드를 PC에 USB High-Speed로 연결해 MSC로 쓰고 읽었다. 기본 경로와 MSC 엔진에서 각각 3 GiB씩이다.
  데이터 검증과 SD 오류 모두 문제없었다.
- 측정값은 [벤치 결과](#벤치-결과), SDR50 진입 과정은 [UHS-I SDR50 동작](#uhs-i-sdr50-동작)에 있다.

아직 확인하지 않은 것:

- Type-A 포트를 PC에 연결할 때의 VBUS 처리(위 주의 참고). 어떤 케이블과 VBUS 연결로 측정했는지는 기록하지 않았다.
- SD 신호선의 실제 1.8 V 진폭(오실로스코프 측정)
- High Speed 50 MHz 동작
