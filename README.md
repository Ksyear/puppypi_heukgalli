# puppypi_heukgalli

**ROG Ally X 를 Hiwonder PuppyPi 전용 핸드헬드 리모컨으로 쓰는 C++ 클라이언트 앱.** 스틱·트리거로 데드맨 주행, 손가락으로 지도에 경로를 그려 보내고, 카메라와 LiDAR 레이더 HUD 를 120 Hz 터치 화면에 띄운다. 로봇 소프트웨어는 한 줄도 바꾸지 않는다.

> **English summary.** A Win32 + Direct3D 11 + Dear ImGui 1.93.0 client for a Hiwonder PuppyPi running a team-written ROS 2 stack, built for the ASUS ROG Ally X handheld ("heukgalli", the author's black Ally X on Windows 11). It speaks only the robot's published network interfaces (UDP Protocol v2, rosbridge WebSocket, HTTP MJPEG/JSON); no Hiwonder or team code is included and the robot software is unchanged. Single static exe via MinGW-w64 GCC 16.2, cross-compiled on macOS or built on the Ally with the in-folder toolchain. The map screen renders the robot's LiDAR occupancy grid (`/map`) with live scans and plans around obstacles on the app side (A* with robot-size inflation, re-planning when the live scan blocks the path), because the robot itself has no obstacle avoidance. Core unit tests (95 checks) pass on macOS, Linux (Ubuntu 24.04) and the Ally; integration scripts pass against a mock robot on the same Mac (drive 14/14, obstacle avoidance 8/8). **Status: runs on the Ally; not yet tested with the real robot.** Hardware testing is tracked in `docs/HW_TEST_CHECKLIST.md`.

---

## 현재 상태 (2026-11-14)

| 항목 | 상태 |
|---|---|
| 코어(프로토콜·파서·LiDAR 지도·경로 계획) | 단위 시험 95개 통과(macOS clang, Ubuntu 24.04 GCC, 흑갈리 MinGW) |
| 통합 시험(가짜 로봇) | 맥 루프백: `tests/it_drive.txt` 14/14, 장애물 회피 `tests/it_avoid.txt` 8/8 통과. 흑갈리 → 맥(Tailscale): `it_drive` 14/14 통과. `it_avoid` 는 Tailscale 이 도쿄 중계(DERP)로 바뀐 때 UDP 유실 25~56% 로 로봇 watchdog(0.4 s)이 경로를 취소해 3개 실패 — 설계대로 멈춘 것이며 실제 LAN 에서 다시 확인 필요 |
| Windows 앱 exe | macOS 교차 빌드와 흑갈리 자체 빌드(`build.ps1`) 모두 성공, 경고 0 |
| 흑갈리에서 실행 | **확인함(가짜 로봇에 연결).** 다섯 화면 표시, 내장 패드 XInput 인식, 화면 120 fps, 프레임당 CPU 평균 0.5–1.0 ms([측정 기록](docs/MEASUREMENTS.md)). 진동·터치 스케치·기울기는 아직 사람이 확인하지 않음 |
| 실제 로봇 시험 | **아직 안 함.** 사용자가 `docs/HW_TEST_CHECKLIST.md` 로 수행 |

가짜 로봇 시험 통과는 실기 검증이 아니다. 아래 모든 "동작한다" 는 서술은 코드와 가짜 로봇 기준이다.

## 화면

흑갈리(ROG Ally X)에서 앱이 직접 저장한 1920×1080 화면이다(F12). **가짜 로봇(`tools/mock_robot.py`)에 연결한 상태**라 카메라는 시험 패턴이고 지도는 가상의 방이다. 주소는 `--privacy` 모드로 가렸다.

| LiDAR 지도 — 점유 격자, 실시간 스캔, 그린 길 → 장애물 회피 경로 | 주행 — 전면 카메라, LiDAR 레이더, 주행 상태 캡슐 |
|---|---|
| ![LiDAR 지도](docs/screenshots/map.png) | ![주행 화면](docs/screenshots/drive.png) |
| **로봇 — 자세 7개, 동작 24개(⚠ 큰 동작은 두 번)** | **로그 — ACK RTT, 유실, 카메라, 화면 fps** |
| ![로봇 화면](docs/screenshots/robot.png) | ![로그 화면](docs/screenshots/log.png) |

디자인 변천(왼쪽: 첫 버전의 기본 ImGui 룩, 오른쪽: "검은 유리 조종석" v2 — 콘텐츠를 화면 전체에 깔고 조작은 떠 있는 유리 조각으로만). 조사·결정 과정은 [`docs/DESIGN_PLAN_v1.md`](docs/DESIGN_PLAN_v1.md), [`docs/DESIGN_PLAN_v2.md`](docs/DESIGN_PLAN_v2.md).

![디자인 전후](docs/screenshots/before_after_drive.png)

<!-- TODO: 실제 로봇 장면 GIF(실기 시험 뒤) -->

더 읽을 것: [아키텍처](docs/ARCHITECTURE.md) · [코드 둘러보기(핵심 5곳)](docs/CODE_TOUR.md) · [측정 기록](docs/MEASUREMENTS.md) · [실기 점검표](docs/HW_TEST_CHECKLIST.md)

## LiDAR 지도와 장애물 회피

로봇의 경로 추종기는 경유점(지도 좌표, 최대 20개)을 차례로 따라갈 뿐 장애물을 피하지 않는다. 그래서 회피는 앱이 맡는다(`src/planner.*`, `Robot::nav_loop`).

1. **지도**: 로봇 지도작성 노드의 `/map`(600×600, 5 cm, 2 s 마다)을 rosbridge 로 받는다. 큰 메시지라 PNG 압축으로 받고(가짜 로봇 기준 1.4 MB → 약 10 KB), 정수 배열은 일반 JSON 해석기를 거치지 않고 직접 읽는다. 앱이 점유 칸(빛나는 청록)·빈 칸·모르는 칸을 직접 그린다. `/lidar_map/pose`(10 Hz)와 `/scan` 을 겹쳐 그린다.
2. **보내기 전**: 손가락으로 그린 선을 경유점으로 단순화한 뒤, 0.10 m 격자에 지도 점유 칸과 실시간 스캔 점을 올려 로봇 크기만큼(0.20 m) 부풀린다. 막힌 구간만 A*(8방향, 벽 가까이는 비싸게)로 돌아가고, 보이는 데까지 건너뛰어 꺾이는 점만 남겨 20개 이하로 줄인다.
3. **가는 중**: 새 스캔마다 남은 경로 앞쪽 1.6 m 를 감시한다. 장애물이 4번 중 3번 잡히면 `path_cancel` → 막힌 점을 20 초 기억 → 현재 위치에서 남은 경유점까지 다시 계획 → 로봇이 중립으로 돌아오면 새 경로를 보낸다.
4. **막혔을 때**: 돌아갈 틈이 없으면 멈춘 채 "우회 경로 없음 — 다시 그려 주세요" 를 띄우고 진동으로 알린다. 사용자가 스틱을 쓰거나 비상정지하면 회피 주행은 즉시 끝난다.

## 왜 Ally X 인가

| Ally X 특성 | 로봇 리모컨에서의 의미 |
|---|---|
| Xbox 배치 패드(스틱 2, 아날로그 트리거 2, 범퍼 2, D-pad, ABXY) | 아날로그 전·후진/회전과 "쥐고 있을 때만 움직이는" 데드맨(RT)을 손을 떼지 않고 구현 |
| 럼블 모터 | 화면을 안 봐도 명령 거절·비상정지·연결 끊김·경로 도착을 손으로 안다 |
| 120 Hz 터치 화면(1920×1080 가로 기준으로 UI 설계) | 카메라 원본 비율 표시, 손가락 경로 스케치, 엄지 도달 범위 안의 큰 버튼 |
| 내장 가속도계 | 기울임 조향 옵션(Windows Sensor API, 실기에서 노출 여부 미확인) |
| Wi-Fi·배터리·한 손 크기 | 로봇 옆을 걸어다니며 조종. 노트북 + 조이스틱 조합을 대체 |
| Windows 11 | Win32·D3D11·XInput 을 그대로 쓰는 단일 exe, 설치 없음 |

## 기능표: 로봇 기능 → 흑갈리 입력·화면

로봇 쪽 인터페이스는 모두 팀 ROS 2 스택이 이미 제공하는 것이다. 앱은 그 클라이언트일 뿐이다.

| 로봇 인터페이스 | 앱 기능 | 흑갈리 입력 / 화면 |
|---|---|---|
| UDP 5002 발견 `PUPPYPI_DISCOVER/1` | 같은 LAN 의 로봇 찾기(nonce 검증) | 연결·설정 화면 "같은 Wi-Fi 에서 찾기" → 목록 터치 |
| 수동 IP | 화면 숫자 패드로 입력, `config.json` 에 기억, 시작 시 자동 재연결 | 연결·설정 화면 |
| UDP 5001 v2 `command` 20 Hz | 데드맨 주행(전·후진, 회전), 속도 비율 | LS Y + RS X, RT 홀드. LT 정밀, LS 클릭 속도 토글 |
| v2 중립 확인 규칙 | 연결 직후·타임아웃 뒤 중립을 보내 `idle` 확인 후에만 주행 | 자동. 상태띠 칩 "대기(중립 확인됨)" / "중립 확인 중" |
| v2 `emergency_stop` | 비상정지(2회 송신) + 아래 rosbridge 전체 정지를 함께 | **B**, 또는 모든 화면 우하단 빨간 버튼 |
| v2 `clear_emergency` | 해제(1 초 길게 누르기) → 재프레스 요구 | 터치 전용 "해제: 1초 누르기" |
| v2 `disconnect` | 중립 1회 + disconnect, UUID 폐기 | 앱 종료, "연결 끊기" 버튼 |
| v2 ACK(8필드 엄격 파싱) | 상태·거절 사유·비상정지 latch 표시, RTT·유실률, 럼블 | 상태띠, 로그 화면, 진동 |
| v2 `path` / `path_cancel` | 지도 위 손가락 스케치 → RDP 단순화 ≤20점 → 전송, 진행 표시 | 지도 화면 드래그 → "경로 보내기". **X** 또는 "경로 취소" |
| rosbridge `/puppy_control/go_home` | 서기(홈) | **A**(RT 를 쥐지 않았을 때), 주행·로봇 화면 버튼 |
| rosbridge `/puppy_control/runActionGroup` | 동작 그룹 24개(`2_legs_stand` 제외). 큰 동작은 2 초 안 두 번 눌러야 실행 | 로봇 화면 격자, 또는 D-pad ◀▶ 선택 + **LB** |
| rosbridge `/puppy_control/pose` 발행 | 자세 프리셋 7개 + 슬라이더(roll/pitch/yaw/height/x_shift/stance/run_time) | 로봇 화면, 또는 D-pad ▲▼(0.3 초 멈춘 자세만 전송) |
| rosbridge `/puppy_control/emergency_stop`·`reset_estop` | 로봇 전체 latch 정지/해제 | B 와 해제 버튼이 v2 와 함께 호출. 로봇 화면 "전체 정지"/"전체 해제" |
| rosbridge `/lidar_mapper/save_map`·`reset_map` | 지도 저장 / 새로 시작(한 번 더 눌러 확인) | 지도 화면 버튼 |
| rosbridge `/puppy_control/status`, `/arbiter_state` | 동작 실행 중·이동 차단 표시 | 상태띠 칩, 로봇 화면 상태 목록 |
| rosbridge `/scan` (throttle 200 ms) | LiDAR 레이더 HUD(반지름 3 m, 0.5 m 격자, 전방 위) | 주행 화면 오버레이. **Y** 로 카메라 / +레이더 / 레이더만 |
| HTTP 8080 카메라 MJPEG | 주행 화면 배경, fps·프레임 나이, 2 초 이상 오래되면 회색 처리 | 주행 화면. 설정에서 끄기 가능 |
| HTTP 8080 지도 JPEG | 지도 화면 배경(×1/×2/×4, 확대 시 로봇 중심) | 지도 화면 |
| (카메라 스트림의 최신 프레임) | 정지 사진을 `shots/cam_*.jpg` 로 저장 | **RB**(기울임 조향 꺼져 있을 때), "사진" 버튼 |
| HTTP 8090 `/api/map` | 픽셀 ↔ 미터 변환(경로 스케치의 근거), 1 Hz | 내부 |
| HTTP 8090 `/api/path` | 경로 상태 idle/rotate/drive/blocked/done/cancelled, 1 Hz(이동 중 4 Hz) | 주행·지도 화면 진행 표시, 럼블 |
| HTTP 8090 `/api/status` | rosbridge 끊겼을 때 중재기 상태 대체원, 2 Hz | 내부 |
| rosbridge `/rosapi/topics` | 연결 진단 | 로그 화면 "토픽 목록" |
| Ally 가속도계 | 기울임 조향(기본 꺼짐, RB 를 쥔 동안만, 쥐는 순간 자세가 0) | 연결·설정 화면 토글 |

## 게임패드 배치

| 입력 | 기능 | 비고 |
|---|---|---|
| 왼쪽 스틱 Y | forward −1..1 | 원형 데드존 0.12(설정 0.05~0.4), 지수 곡선 1.5 |
| 오른쪽 스틱 X | turn −1..1 (오른쪽 = 우회전) | "왼쪽 스틱 하나로 운전" 옵션이면 LS X |
| **RT ≥ 50 %** | **데드맨.** 쥐고 있을 때만 `control_active:true`. 놓으면 즉시 중립 | 더 깊게 쥘수록 빠름(50 % → 100 %) |
| LT ≥ 50 % | 정밀 모드 ×0.4 | |
| A | go_home(서기) | RT 를 쥐고 있으면 무시 |
| **B** | **비상정지**(v2 + rosbridge 전체 정지) | 어느 화면에서나. 해제는 터치 전용 |
| X | 경로 취소 | |
| Y | HUD 레이어 순환(카메라 → 카메라+레이더 → 레이더만) | |
| D-pad ▲▼ | 자세 프리셋 순환, 0.3 초 뒤 자동 발행 | RT 를 쥐고 있으면 발행 안 함 |
| D-pad ◀▶ | 동작 그룹 선택 | 왼쪽 띠에 이름 표시, 큰 동작은 노랑 |
| LB | 선택한 동작 그룹 실행 | 큰 동작은 2 초 안 두 번. RT 를 쥐고 있으면 무시 |
| RB | 사진 저장. 기울임 조향이 켜져 있으면 RB 홀드 중 기울기로 운전 | |
| LS 클릭 | 최고 속도 100 % ↔ 설정값(기본 60 %) | |
| RS 클릭 | 없음 | |
| View | 화면 전환 주행 → 지도 → 로봇 → 로그 | |
| Menu | 연결·설정 화면 ↔ 주행 | 연결·설정 화면에서는 운전하지 않음 |
| View + Menu 1.5 초 | 앱 종료 | |
| 키보드 W/S/A/D | 패드가 없을 때만 개발용 운전(0.6 배) | F11 전체 화면 토글, `--windowed` 인자 |

M1/M2 뒷면 버튼은 Armoury Crate 설정에 따라 XInput 으로 보이지 않을 수 있어 매핑하지 않았다.

## 안전 모델

로봇이 움직이는 조건은 매 50 ms 송신 주기마다 순수 함수 `decide_drive()` 가 다시 판단한다(`src/v2client.cpp`, 단위 시험 대상). 활성 명령(`control_active:true`)은 **아래 전부가 참일 때만** 나간다. 나머지 경우는 모두 중립(`control_active:false, forward:0, turn:0`)이다.

1. **데드맨**: RT ≥ 50 %(또는 RB 기울임, 키보드)를 쥐고 있고 스틱이 0 이 아니다.
2. **입력 신선도**: UI 스레드가 최근 **0.25 s** 안에 입력을 넘겼다. 화면이 멈추면 중립.
3. **포커스**: 창이 활성(`WM_ACTIVATEAPP`), 최소화·가려짐(DXGI occluded) 아님, 연결·설정 화면 아님. 포커스를 잃는 순간 `set_drive(0,0,false)` 를 바로 호출한다.
4. **ACK 신선도**: 로봇 ACK 가 최근 **0.6 s** 안에 왔다. 로봇 쪽 watchdog(0.4 s)이 먼저 멈추고, 앱은 그 뒤 중립으로 돌아간다.
5. **로봇 상태**: 마지막 ACK 의 `state` 가 `idle` 또는 `active`. `awaiting_neutral`·`timeout` 이면 중립을 보내며 확인을 기다리고, 로봇이 `idle` 을 돌려주면 자동 재개한다.
6. **비상정지 없음**: 앱이 비상정지를 요청한 상태도, ACK 의 `emergency_stop` latch 도 아니다.
7. **재프레스 규칙**: 비상정지 해제 직후에는 `release_required` 가 켜진다. 데드맨을 **한 번 놓아야** 풀리고, 놓기 전까지는 쥐고 있어도 움직이지 않는다.

**비상정지(B 또는 빨간 버튼)** 는 v2 `emergency_stop` 을 두 번(번호를 올려) 보내고, 동시에 rosbridge `/puppy_control/emergency_stop` 으로 로봇 전체 latch 도 건다. 긴 진동 0.6 s. **해제**는 터치 전용으로 1 초 길게 눌러야 하며, rosbridge `reset_estop` 완료 콜백 뒤에 v2 `clear_emergency` 를 보낸다(순서는 팀 로봇 문서 06 §3 의 latch 관계에 따름). 그 뒤 재프레스 규칙이 적용된다.

**로봇 쪽 뒷받침**: 단일 조종자(UUID + 주소, lease 1.0 s), watchdog 0.4 s, 활성 명령 전 중립 확인, 비상정지 latch 는 조종자만 해제, disconnect 한 UUID 재사용 불가. 앱이 죽거나 Wi-Fi 가 끊겨도 로봇은 0.4 s 안에 스스로 멈춘다. 앱 종료 시에는 중립 1회 + `disconnect` 를 보내 소유권을 즉시 반납하고 다음 연결은 새 UUID 를 쓴다.

**손으로 아는 상태**(럼블): 짧게 = 수락·동작 시작·사진, 두 번 = 거절(`not_owner`, `neutral_required` 등, 1 초에 한 번)·연결 끊김·경로 취소/차단, 길게 = 비상정지, 두 박자 = 경로 도착.

## 아키텍처

`src/` 코어(json·util·sock·net·rosbridge·v2client·discovery·path_tools·robot)는 표준 라이브러리와 소켓만 쓰는 크로스플랫폼 코드로, `sock.hpp` 한 곳에서 Winsock/POSIX 를 덮는다. `src/win/` 은 Win32 창·D3D11·ImGui·XInput·Sensor API 를 다루는 Windows 전용 층이다. 화면 스레드는 매 프레임 `Robot::snapshot()` 을 한 번 복사해 읽기만 하고, 로봇을 움직이는 요청은 메서드 호출로 넘긴다. 실제 송신은 v2 송신 스레드(20 Hz) 한 곳에서만 한다. JSON 파서, WebSocket 프레이밍, MJPEG 분리, RDP 단순화는 외부 의존성 없이 직접 구현했다(UI 는 Dear ImGui, JPEG 디코드는 stb_image).

스레드 구성(UI 120 Hz, v2 송신 20 Hz + 수신, rosbridge, MJPEG ×2, JPEG 디코더 ×2, HTTP 폴러, 기울기 센서), v2 클라이언트 상태 규칙, 로봇 포트·엔드포인트는 **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** 에 Mermaid 다이어그램으로 정리했다.

## 빌드·실행

CMake 없이 `Makefile` 하나와 PowerShell 래퍼로 두 플랫폼을 다룬다. C++17.

### macOS (코어 시험, 교차 빌드)

```sh
make test                                  # clang++, 단위 시험 빌드·실행 → build/test_core
make console                               # 텍스트 클라이언트 → build/console
make CROSS=x86_64-w64-mingw32- BUILD=build-win app   # Windows exe 교차 빌드 → build-win/puppypi_heukgalli.exe
```

교차 빌드에는 `x86_64-w64-mingw32-g++`·`windres` 가 PATH 에 있어야 한다. 결과는 `-static -static-libgcc -static-libstdc++` 로 링크한 단일 exe(의존 DLL 없음)이며, 아이콘·매니페스트는 `src/win/app.rc` 로 포함된다.

### 흑갈리 (Windows 11, 폴더 안 툴체인)

1. WinLibs MinGW-w64 GCC 16.2 UCRT 를 `toolchain\mingw64\` 에 풀어 둔다(시스템 설치·PATH 변경 없음, `.gitignore` 대상).
2. Git for Windows 가 `C:\Program Files\Git\usr\bin\sh.exe` 를 제공해야 한다(Makefile 의 `mkdir -p`·`rm -rf` 용).
3. PowerShell:

```powershell
.\build.ps1          # 앱 → build\puppypi_heukgalli.exe
.\build.ps1 test     # 코어 단위 시험
.\build.ps1 run      # 빌드 후 실행
```

실행 시 테두리 없는 전체 화면으로 뜨며(F11 로 창 모드, `--windowed` 인자로 시작부터 창 모드, `--privacy` 는 화면에서 IP 를 가림, `--kbdrive` 는 패드 없는 PC 에서 Shift 를 쥔 동안만 W/A/S/D 운전, F12 는 화면 저장), 설정은 exe 옆 `config.json`, 사진은 `shots\` 에 저장된다. 한글은 시스템 `malgun.ttf`(+ 기호 `seguisym.ttf`)를 실행 시 읽어 쓰므로 폰트를 재배포하지 않는다. 마지막으로 연결한 주소가 있으면 시작과 함께 자동 연결을 시도한다.

**주의**: 서명하지 않은 exe 라 Windows Smart App Control 이 켜져 있으면 실행이 막힌다(앱별 예외가 없다). 작성자는 개발용 기기에서 이 기능을 끄고 실행했다. 끄기 전에 Windows 업데이트에 따라 다시 켜기 어려울 수 있다는 점을 확인할 것.

## 테스트

### 단위 시험 (`tests/test_core.cpp`, 외부 프레임워크 없음)

`make test` → **95 checks pass** (macOS clang, Ubuntu 24.04, 흑갈리에서 `.\\build.ps1 test` 도 같음). 막힘 판정(4번 중 3번·쿨다운)과 남은 경유점 계산도 시험한다. LiDAR·계획 시험: 점유 격자 빠른 해석, 쿼터니언→yaw, 스캔→지도 변환, 직선 막힘 우회, 벽 틈 통과, 완전히 막힘 실패, 경유점이 장애물 안이면 실패, 점유 격자 지도 기반 우회, 앞쪽 감시 범위. MinGW 로도 컴파일된다(`build-win/test_core.exe`). 대상: JSON 파서 왕복·깊이 제한·정수/실수 출력, ACK 8필드 엄격 파싱, `decide_drive()` 전이 표(중립 확인·데드맨·입력 정지·ACK 끊김·estop·timeout 자동 재개·재프레스), 발견 응답 nonce/형식 검증, 지도 픽셀↔미터 변환, RDP 스케치 단순화(직선→2점, L→3점, 지그재그→≤20점), `/api/map`·`/api/path`·`/scan` 파서, UUID/base64/문자열 도구.

### 통합 시험 (가짜 로봇, 같은 Mac 루프백)

```sh
git clone https://github.com/Ksyear/PuppyPI_Cpp_ROS2   # 가짜 로봇이 규칙을 빌려 오는 로봇 저장소
export PUPPYPI_ROBOT_SRC=$PWD/PuppyPI_Cpp_ROS2/ros2_ws/src/puppy_vr_bridge
python3 tools/mock_robot.py                       # UDP 5001/5002, TCP 8080/8090/9090 을 127.0.0.1 에 연다
./build/console --host 127.0.0.1 --script tests/it_drive.txt
```

`tests/it_avoid.txt`(8/8, 맥 루프백): 경로 위에 미리 놓인 장애물을 피해 도착, 가는 중에 경로 위에 생긴 장애물을 스캔으로 찾아 다시 계획해 도착, 두 경우 모두 가짜 로봇의 충돌 기록 없음.

`tests/it_drive.txt` 는 연결 → 중립 확인 → 주행 → 놓기 → go_home → 자세 → 동작 그룹 → 경로 전송(rotate→drive→done) → 스틱 개입으로 경로 취소 → 비상정지 → 해제 → 지도 저장/새로 → 토픽 목록 → RTT 수집을 차례로 돌리며, **expect 14/14 통과**(실패 시 종료 코드 1).

`tools/mock_robot.py` 는 Python 표준 라이브러리로 만든 **시험 보조 도구**다(앱 자체는 C++). 실행 시 `--robot-src`(또는 `PUPPYPI_ROBOT_SRC`)가 가리키는 [로봇 저장소](https://github.com/Ksyear/PuppyPI_Cpp_ROS2)의 프로토콜 모듈(`protocol_v2.py`, `path_follower.py`)을 import 해서 조종 규칙의 기준으로 쓴다. 로봇 코드는 이 저장소에 복사하지 않았다. `discovery.py` 가 없으면 문서의 응답 형식으로 대신한다. 자체 시험: `python3 tools/test_mock_robot.py`.

## 측정값

| 항목 | 값 | 조건 |
|---|---|---|
| ACK RTT (명령 송신 → ACK 수신, 한 기기 단조 시계) | 중앙값 **0.39 ms**, p95 **0.72 ms**, n = 61 | **macOS 루프백(127.0.0.1) + `tools/mock_robot.py`**, `it_drive.txt` 의 `rtt 3`. 실제 로봇도, Wi-Fi 도 아니다. 네트워크·로봇 처리 지연을 전혀 포함하지 않는다 |
| ACK RTT, 흑갈리 → Mac 의 가짜 로봇 (Tailscale) | 중앙값 **17.87 ms**, p95 **20.96 ms**, n = 399 | 흑갈리 앱 로그 화면 값(2026-11-14). 같은 집 안 두 기기 사이 Tailscale 경로이며 로봇 처리도, 로봇 Wi-Fi 도 아니다 |
| 화면 갱신, JPEG 풀기, 프레임 나이 (흑갈리) | 120 fps, 0.8 ms, 51 ms | 같은 조건. 카메라는 가짜 로봇의 320×240 영상이라 실제 1280×720 카메라와 다르다 |
| 실제 Wi-Fi ACK RTT (흑갈리 ↔ PuppyPi) | **미측정** | 실기 점검표 2번 이후 `docs/MEASUREMENTS.md` 에 기록 예정 |
| 카메라 수신 fps | **미측정** | 로그 화면에 표시되지만 실기에서 읽은 값 없음 |
| 카메라 프레임 나이(수신 → 화면 표시) | **미측정** | 같음 |

스틱 입력 → 로봇 다리 움직임까지의 종단 지연은 로봇 시계와의 동기화 근거가 없어 측정 대상에서 뺐다. 앱이 보여 주는 RTT 는 흑갈리 한 기기의 시계로 잰 왕복 시간이다.

## 한계와 범위 밖

- **실제 로봇과는 아직 연결하지 않았다.** 흑갈리에서 D3D11·한글 폰트·내장 패드 인식은 확인했지만, 진동·터치 스케치·기울기 센서와 실제 로봇 동작은 점검표 결과를 기다린다.
- 가짜 로봇은 팀 모듈을 쓰더라도 시뮬레이션이다. ACK 지연, 경로 운동학, 두 비상정지 latch(v2·rosbridge)의 해제 순서는 실기에서 확인해야 한다(점검표 13번).
- 인증·암호화 없음. 로봇 인터페이스가 평문 LAN 전제이므로 같은 Wi-Fi 안에서만 쓴다.
- 로봇은 경로 추종 중 장애물을 피하지 않는다. 회피는 앱이 맡는다: LiDAR 스캔·지도로 막힘을 판단하고 경로를 취소한 뒤 다시 보낸다. 판단 지연(스캔 4번 중 3번, 약 0.3~0.4 s)과 Wi-Fi 지연 동안 로봇은 계속 움직이므로, 아주 가까이에서 갑자기 나타난 장애물은 피하지 못할 수 있다. 앞쪽 1.6 m 안만 감시한다. 실기 검증 전.
- ImGui 터치는 단일 터치 드래그만. 핀치 확대 없음(−/+ 버튼).
- Armoury Crate 가 패드를 데스크톱(마우스/키보드) 모드로 두면 XInput 입력이 없다. M1/M2 뒷버튼 미매핑.
- 가속도계는 Windows Sensor API 에 노출되지 않을 수 있다. 그 경우 설정에 "가속도계 없음" 으로 뜨고 기능은 꺼진다.
- ACK `out_of_order` 거절은 표시만 하고 자동 재동기화는 하지 않는다. 사용자가 "연결 끊기 → 연결" 로 새 세션을 만든다.
- 로봇 쪽 변경이 필요한 것(카메라 해상도/비트레이트, 타임스탬프 동기화, 로봇 자체의 장애물 회피)은 범위 밖.
- 지도 PNG 압축(`compression:"png"`)을 로봇 rosbridge 가 지원하지 않으면 8 초 뒤 일반 JSON(약 1.4 MB / 2 s)으로 바꾼다. 실기에서 어느 쪽인지 로그 화면 "지도" 줄에 표시된다.
- CI 없음. GitHub 공개 여부와 함께 결정 예정.

## 배운 점 (초안 — 사용자가 다듬을 후보)

> 아래는 코드를 쓰면서 나온 후보 문장이다. 실기 결과가 들어오면 바뀔 수 있다.

- 소유권·lease·watchdog·중립 확인이 있는 UDP 프로토콜에서, 클라이언트의 "움직일 것인가" 판단을 입력·시계·ACK 를 모두 받는 순수 함수 하나로 모으면 단위 시험이 쉬워지고 안전 규칙이 한 곳에 보인다.
- 데드맨은 트리거만이 아니다. 입력 신선도, 창 포커스, 화면 가림, ACK 신선도가 모두 데드맨의 일부여야 핸드헬드에서 안전하다.
- 비상정지 해제 뒤의 재프레스 규칙처럼 "사용자가 의도를 다시 보여야 하는" 전이는 상태 비트 하나로 표현되지만, UI 문구로 알려 주지 않으면 고장처럼 느껴진다.
- WebSocket(rosbridge)·MJPEG multipart·JSON 을 표준 라이브러리만으로 직접 구현해도 범위가 좁으면 짧게 끝난다. 대신 ACK 8필드처럼 "허용 형식만 받는다" 를 분명히 해야 한다.
- 엄지 도달 범위(좌우 띠 + 하단)와 96 px 이상 버튼으로 ImGui 를 터치 UI 로 쓰는 것은 가능하지만, 멀티터치가 없다는 제약은 설계 초기에 받아들여야 한다.
- MinGW 정적 링크 단일 exe 는 배포가 쉽지만, 서명이 없으면 Smart App Control 에 막힌다는 것을 실행 전 단계에서 배웠다.
- 측정은 조건을 같이 적지 않으면 의미가 없다. 루프백 0.39 ms 는 Wi-Fi 지연과 아무 관계가 없고, 종단 지연은 아예 측정할 수 없었다는 점을 그대로 적는 쪽이 정직하다.

## 라이선스와 서드파티

- 이 저장소의 코드: **MIT License** (© 2026 Ksyear). 전문은 [`LICENSE`](LICENSE).
- 포함된 서드파티 — [`THIRD_PARTY.md`](THIRD_PARTY.md) 참조:
  - Dear ImGui 1.93.0 (MIT) — `third_party/imgui/`, 코어 + Win32/DX11 백엔드, 무수정
  - stb_image 2.32 (MIT 또는 Public Domain) — `third_party/stb_image.h`, 무수정
  - 맑은 고딕·Segoe UI Symbol — Windows 시스템 폰트를 실행 시 읽음, 재배포하지 않음
- 포함하지 않은 것: Hiwonder PuppyPi 소프트웨어, 팀의 로봇·VR 코드. 앱은 공개된 네트워크 인터페이스(UDP Protocol v2, rosbridge, HTTP)만 쓰는 클라이언트다. `tools/mock_robot.py` 는 로봇 저장소의 모듈을 실행 시 import 할 뿐 그 코드를 재배포하지 않는다.
- 툴체인: WinLibs MinGW-w64 GCC 16.2 UCRT(재배포하지 않음). 정적 링크 결과물은 GCC 런타임 라이브러리 예외에 따라 GPL 적용 대상이 아니다.
