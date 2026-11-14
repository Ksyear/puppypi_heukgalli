# puppypi_heukgalli 계획 v1

작성: 2026-11-14 | 상태: 초안, 사용자 결정 대기 항목은 **[결정]** 표시
대상: ASUS ROG Ally X(흑갈리)에서 Hiwonder PuppyPi(ROS 2 Humble)를 제어하는 개인 C++ 포트폴리오 앱

## 0. 한 줄 요약

흑갈리를 "로봇 전용 핸드헬드 리모컨"으로 쓴다. 스틱·트리거로 데드맨 주행, 터치로 지도에 경로를 그려 보내고, 카메라+LiDAR 레이더 HUD를 120 Hz 화면에 띄운다. 로봇 소프트웨어는 한 줄도 바꾸지 않고 공개된 네트워크 인터페이스(UDP v2, rosbridge, HTTP)만 쓰는 **클라이언트 전용** 앱이다.

## 1. 제품 콘셉트

### 왜 Ally X가 좋은 로봇 리모컨인가

| Ally X 특성 | 로봇 제어에서의 의미 |
|---|---|
| Xbox 배치 패드(스틱 2, 트리거 2, 범퍼 2, D-pad, ABXY) | 아날로그 전·후진/회전과 "쥐고 있을 때만 움직이는" 데드맨을 손을 떼지 않고 구현 |
| 럼블 모터 | 화면을 안 봐도 ACK 거절·비상정지·소유권 상실을 손으로 느낌 |
| 7" 1920×1080 120 Hz 터치 | 카메라 1280×720 원본 비율 표시 + 터치로 경로 스케치. HUD 갱신이 부드러움 |
| 가속도계(Bosch) | 기울임 조향 실험(옵션). API 가용성 미확인 → 리스크 |
| Wi-Fi ac, 배터리, 한 손 크기 | 로봇 옆에서 걸어다니며 조종. 노트북+조이스틱 조합을 대체 |
| Zen 4 8코어 + 23 GB | MJPEG 디코드·ImGui 렌더·네트워크 스레드를 넉넉히 돌림 |

### 시그니처 기능(현실적인 2개)

1. **데드맨 주행 + 햅틱 ACK**: RT를 쥐어야 움직이고, 놓으면 즉시 중립. 로봇 ACK의 `accepted/reason/state`가 럼블 패턴으로 바뀌어 "로봇이 내 명령을 받았는지"를 손으로 안다. 화면에 ACK RTT 실측값을 띄운다.
2. **지도 위 터치 경로 스케치**: `/lidar_map/image`(1 fps) 위에 손가락으로 선을 그리면 RDP 단순화로 ≤20점 경로를 만들어 `path`로 전송. 로봇의 `/api/path` 진행 상태(index·remaining_m)를 지도 위에 겹쳐 보여주고, 스틱을 건드리면 즉시 취소되는 안전 규칙을 UI에도 반영.

보조: 카메라 위 **LiDAR 레이더 HUD**(`/scan` 450빔을 5 Hz로 구독해 반투명 극좌표 오버레이). 구현이 쉽고 화면이 눈에 띄어 포트폴리오 스크린샷에 유리하다.

## 2. 기능 목록과 입력 매핑

### 2.1 로봇 기능 → 앱 기능 전체 매핑

| # | 로봇 기능(인터페이스) | 앱 기능 | 입력/UI | 단계 |
|---|---|---|---|---|
| F01 | 발견 UDP 5002 `PUPPYPI_DISCOVER/1` | LAN 브로드캐스트 검색, 목록 표시 | 연결 화면 "검색" 버튼 | MVP |
| F02 | 수동 IP | IP 직접 입력, 최근 주소 기억(`config.json`) | 연결 화면 텍스트 상자 | MVP |
| F03 | v2 `command` 20 Hz | 데드맨 주행(전·후진, 회전) | LS Y + RS X + RT 홀드 | MVP |
| F04 | v2 중립 확인 규칙 | 자동 중립 전송 → `state:idle` 확인 후 주행 허용 | 자동, HUD에 상태 표시 | MVP |
| F05 | v2 `emergency_stop` 이벤트 | 비상정지(소유자 래치) | B 버튼 + 화면 우하단 큰 빨간 버튼 | MVP |
| F06 | v2 `clear_emergency` | 비상정지 해제(2단계 확인) | 터치 전용(길게 누르기 1 s) | MVP |
| F07 | v2 `disconnect` | 정상 종료 시 UUID 반납 | 앱 종료, "연결 끊기" 버튼 | MVP |
| F08 | ACK 8필드 | 상태 머신 입력, RTT 측정, 거절 사유 표시·럼블 | HUD, 럼블 | MVP |
| F09 | v2 `path` / `path_cancel` | 지도 터치 스케치 → 경로 전송, 진행 표시, 취소 | 지도 화면 터치, X 버튼 = 취소 | M5 |
| F10 | rosbridge `/puppy_control/go_home` | "서기(홈)" | A 버튼, 로봇 화면 버튼 | M4 |
| F11 | rosbridge `/puppy_control/runActionGroup` | 24개 동작 그룹 격자(2_legs_stand 제외) | 로봇 화면 격자, D-pad ◁▷ + LB 실행 | M4 |
| F12 | rosbridge `/puppy_control/pose` 발행 | 7개 자세 프리셋 + 슬라이더(roll/pitch/yaw/height/x_shift/stance) | 로봇 화면, D-pad △▽ 프리셋 순환 | M4 |
| F13 | rosbridge `/puppy_control/emergency_stop`·`reset_estop` | 로봇 전체 래치 비상정지/해제 | 큰 빨간 버튼(F05와 동시 발사 **[결정]**), 해제는 설정 화면 | M4 |
| F14 | rosbridge `/lidar_mapper/save_map`·`reset_map` | 지도 저장/초기화(확인 대화상자) | 지도 화면 버튼 | M4 |
| F15 | rosbridge `/puppy_control/status`, `/arbiter_state` | 누가 움직일 권한인지, 동작 그룹 실행 중 차단 표시 | HUD 상단 띠 | M4 |
| F16 | rosbridge `/scan` 10 Hz | 레이더 HUD(5 Hz throttle) | 주행 화면 오버레이, Y로 토글 | M4 |
| F17 | rosbridge `/lidar_map/pose`, `/map` | 로봇 자세 표시. `/map` OccupancyGrid는 JSON이 커서 **사용하지 않음**(JPEG+`/api/map`으로 대체) | 지도 화면 | M5 |
| F18 | HTTP 8080 카메라 MJPEG | 주행 화면 배경, fps·프레임 나이 표시 | 주행 화면 | MVP |
| F19 | HTTP 8080 지도 JPEG 1 fps | 지도 화면 배경 | 지도 화면 | M5 |
| F20 | HTTP 8080 `/snapshot` | 카메라 정지 사진 저장(`shots/`) | RB 버튼 | M6 |
| F21 | HTTP 8090 `/api/map` | 픽셀↔미터 변환(경로 스케치의 근거) | 내부 | M5 |
| F22 | HTTP 8090 `/api/status` | rosbridge 끊겼을 때 대체 상태원 | 내부, 2 Hz | M4 |
| F23 | HTTP 8090 `/api/path` | 경로 상태(idle/rotate/drive/blocked/done/cancelled) | 지도·HUD, 5 Hz | M5 |
| F24 | rosbridge `rosapi` | 연결 진단(토픽 목록) | 로그 화면 "진단" | M6 |
| F25 | 가속도계(Ally) | 기울임 조향(옵션, 기본 꺼짐) | 설정 토글, RB 홀드 중만 적용 **[결정]** | M6 |

로봇 변경이 필요해서 **범위 밖/미래**로 두는 것: 인증·암호화(평문 LAN 전제), 장애물 회피, 카메라 해상도/비트레이트 변경(`type=mjpeg&width=640&quality=60`은 서버 재인코딩이라 Pi CPU 부담 → 사용자 실기 확인 후 선택), 로봇 쪽 타임스탬프 동기화.

### 2.2 게임패드 전체 배치

| 입력 | 기능 | 비고 |
|---|---|---|
| 왼쪽 스틱 Y | forward −1..1 | 데드존 0.12, 지수 커브 1.5 **[결정: 단일 스틱 모드 제공?]** |
| 오른쪽 스틱 X | turn −1..1 | 데드존 0.12 |
| RT (홀드 ≥ 50%) | **데드맨**. 쥐고 있을 때만 `control_active:true`. 놓으면 즉시 중립 | 트리거 값은 속도 상한에도 곱함(살짝 쥐면 느림) |
| LT (홀드) | 정밀 모드 ×0.4 | |
| A | go_home(서기) | 주행 중(RT 홀드)에는 무시 |
| **B** | **비상정지**(v2 estop + rosbridge estop **[결정]**) | 어느 화면에서나 동작. 해제는 터치 전용 |
| X | path_cancel | 경로 없으면 무시 |
| Y | HUD 레이어 순환(카메라 → 카메라+레이더 → 레이더만) | |
| D-pad △▽ | 자세 프리셋 선택 순환, 0.3 s 뒤 자동 발행 | Stand/LieDown/LookDown·10·20·30/StandLow |
| D-pad ◁▷ | 동작 그룹 선택 순환(화면에 이름 표시) | |
| LB | 선택한 동작 그룹 실행 | 실행 중 arbiter가 이동을 막는 점을 HUD에 표시 |
| RB | 스냅샷 저장. 기울임 조향 켜져 있으면 홀드 중 RS X 대신 기울임 사용 | |
| LS 클릭 | 최고 속도 단계 토글(100% / 60%) | |
| RS 클릭 | 예약(없음) | |
| View(≡≡) | 화면 전환 주행 → 지도 → 로봇 → 로그 | |
| Menu(≡) | 설정/연결 화면 | |
| M1/M2(뒷면) | XInput으로 보이면 B와 같은 비상정지 중복 매핑 | Armoury Crate 기본 설정에서 키보드 매크로일 가능성 → 미확인 |
| Armoury Crate/Command Center 키 | OS 전용, 사용 안 함 | 앱 포커스 상실 → 중립 |

### 2.3 터치 UI 화면(1920×1080 가로, 엄지 도달 범위 우선)

화면 좌우 240 px 띠가 양 엄지 영역이다. 중앙은 보기 전용, 조작 버튼은 양쪽 띠와 하단에 둔다. 버튼 최소 96×96 px.

**주행 화면**
```
┌────────────────────────────────────────────────────────────────────────────────────┐
│ 상태띠: 로봇이름 192.168.x.x │ v2: active │ 소유자 ✓ │ RTT 7 ms │ 카메라 29 fps │ rosb ✓ │ ⚠ arbiter: action │
├──────────┬──────────────────────────────────────────────────────────────┬──────────┤
│ [지도]   │                                                              │ [레이더] │
│ [로봇]   │                 카메라 1280×720 (16:9 맞춤)                  │ [스냅샷] │
│ [로그]   │                                                              │          │
│          │        ┌────────── 레이더 HUD(반투명 극좌표) ──────────┐       │ 속도 60% │
│ 자세:    │        │   · 로봇 중심, 0.5 m 격자, 전방 위           │       │ 정밀 ○   │
│ Stand    │        └──────────────────────────────────────────────┘       │          │
│ 동작:    │                                                              │          │
│ wave     │   fwd ▮▮▮▮▯▯  turn ◁▮▮▯▯▷        데드맨 ● RT 쥐는 중        │ ┌──────┐ │
│          │                                                              │ │ 비상 │ │
│          │                                                              │ │ 정지 │ │
│          │                                                              │ └──────┘ │
├──────────┴──────────────────────────────────────────────────────────────┴──────────┤
│ 이벤트 3줄: 12:01:03 ACK rejected neutral_required → 중립 재확인 …                   │
└────────────────────────────────────────────────────────────────────────────────────┘
```

**지도 화면**
```
┌────────────────────────────────────────────────────────────────────────────────────┐
│ 상태띠(동일)                                                                        │
├──────────┬──────────────────────────────────────────────────────────────┬──────────┤
│ [주행]   │  지도 JPEG(흰 자유/검 벽/회 미지, 빨강 로봇+화살표, 파랑 궤적)  │ [경로    │
│ [로봇]   │  + 손가락 스케치(노랑 선) → 단순화 점(초록 ●, 최대 20)          │  전송]   │
│          │  + 로봇 진행 index 강조, remaining 2.3 m                       │ [경로    │
│ 확대 −/+ │                                                              │  취소]   │
│ 지도저장 │                                                              │ [지우기] │
│ 지도초기화│                                                              │          │
│ (확인창) │                                                              │ ┌──────┐ │
│          │  경로상태: drive 3/7  elapsed 12 s                            │ │ 비상 │ │
│          │                                                              │ │ 정지 │ │
├──────────┴──────────────────────────────────────────────────────────────┴──────────┤
│ 안내: 전송 중엔 스틱을 건드리면 경로가 취소됩니다 · 로봇은 장애물을 피하지 않습니다     │
└────────────────────────────────────────────────────────────────────────────────────┘
```

**로봇 화면**: 왼쪽 7개 자세 프리셋 큰 버튼 + 6개 슬라이더(roll/pitch/yaw, height/x_shift/stance_x·y, run_time), 오른쪽 24개 동작 그룹 4×6 격자(2_legs_stand 없음, 위험·장시간 동작은 노란 테두리 **[결정: 어떤 동작을 "확인 필요"로 할지]**), 하단 go_home·rosbridge 비상정지/해제. 비상정지 버튼은 항상 우하단.

**연결/설정 화면**: 검색 결과 목록(이름·IP·id), IP 입력, 연결/끊기, 옵션(단일 스틱, 기울임 조향, 속도 상한, 데드존, HUD 밝기, 카메라 스트림 종류), 폰트·버전·THIRD_PARTY 표시.

**로그 화면**: LogRing 이벤트, 원시 datagram sniff(송·수신 JSON), 측정값(RTT 히스토그램, 전송 주기 지터, 프레임 나이).

## 3. 아키텍처

### 3.1 모듈

```
src/
  core/ (크로스플랫폼, Mac에서 테스트)
    json.*  util.*  sock.*  net.*  rosbridge.*      ← 작성됨(미컴파일)
    v2_client.*     UDP 5001 송수신, 시퀀스, ACK 매칭, RTT
    session.*       v2 세션 상태 머신(3.3절), 순수 함수 + 작은 클래스
    discovery.*     UDP 5002 nonce 검증, 응답 파서
    path_tools.*    RDP 단순화, 최소 간격, ≤20점, 픽셀↔미터(/api/map)
    robot_model.*   UI가 읽는 스냅샷(상태, 자세, 경로, 측정값) + 뮤텍스
    media_api.*     /api/map·status·path 폴링 파서
    actions.*       24개 동작 이름·7개 프리셋 상수 표
  win/ (Windows 전용)
    app_main.cpp    Win32 창, DX11 스왑체인, ImGui 루프, 포커스 처리
    input_xinput.*  패드 폴링, 데드존·커브, 연결 끊김 감지, 럼블 큐
    sensor_tilt.*   Windows Sensor API(COM) 가속도계, 실패 시 기능 비활성
    textures.*      JPEG → stb_image → D3D11 텍스처(카메라·지도)
    ui_*.cpp        화면별 ImGui 코드(drive, map, robot, connect, log)
    fonts.*         한글 폰트 로드와 글리프 범위
  cli/
    console.cpp     Mac/Windows 공용 텍스트 클라이언트(통합 테스트·측정용)
tests/              단위 테스트(자체 미니 assert)
tools/              mock_robot.py(테스트 픽스처), 측정 스크립트
third_party/        imgui(MIT), stb_image(MIT/Unlicense) + THIRD_PARTY.md
docs/               PLAN, ARCHITECTURE(다이어그램), HW_TEST_CHECKLIST, MEASUREMENTS
```

### 3.2 스레드와 데이터 흐름

| 스레드 | 주기 | 역할 |
|---|---|---|
| UI(메인) | 120 Hz vsync(배터리 옵션 60) | Win32 메시지, XInput 샘플링은 하지 않음(아래), ImGui 그리기, RobotModel 스냅샷 복사 |
| control | 100 Hz 틱, 송신 20 Hz | XInput 폴링 → 데드맨·중립 판단 → `command` 송신, ACK 수신(논블로킹), 세션 상태 머신 진행, 럼블 명령 발행. 우선순위 ABOVE_NORMAL |
| rosbridge | 이벤트 | 기존 RosBridge 스레드. 토픽 콜백은 RobotModel에만 쓰고 UI를 직접 건드리지 않음 |
| mjpeg×2 | 스트림 | 카메라·지도 MjpegReader(최신 1장만 보관) |
| http_poll | 2–5 Hz | `/api/path`(경로 중 5 Hz), `/api/map`(지도 화면에서 1 Hz), `/api/status`(2 Hz) |
| discovery | 요청 시 | 브로드캐스트 후 1.5 s 수집 |

흐름: 입력 → control 스레드(단일 송신자, 시퀀스 단조 증가) → UDP → ACK → `Session::on_ack()` → RobotModel 갱신 → UI 표시·럼블. rosbridge/HTTP 수신은 RobotModel의 별 필드에만 쓴다. UI는 프레임마다 `RobotModel::snapshot()` 한 번 복사하고 쓰지 않는다(의도는 `Intent` 큐로 control 스레드에 전달: estop, path 전송, 설정 변경).

### 3.3 v2 세션 상태 머신(클라이언트 측)

```
 Disconnected ──connect()──▶ Connecting(소켓 생성, UUID 새로 발급)
     ▲                              │ 첫 중립 송신
     │                     AwaitNeutral ──ACK accepted & state idle──▶ Idle
     │ disconnect()/종료            ▲        ▲                          │ RT 홀드 & 스틱 ≠ 0
     │                              │        │ ACK reason=neutral_required│
     │                              │        │ 또는 state=timeout         ▼
     │                              │        └──────────────────────── Active ──RT 놓음──▶ Idle(중립 송신)
     │                              │
     │                              │ clear_emergency 수락 뒤 중립 재확인
     │                           EStopped ◀── B/터치 estop, 또는 ACK emergency_stop=true
     │
     ├── OwnerLost: ACK not_owner → 2 s마다 중립 재시도(상대 lease 1.0 s 만료 시 회수). HUD에 "다른 클라이언트 소유" 표시
     ├── Fault: ACK state=fault → 송신 유지(중립), 경고 표시, 사용자에게 로봇 확인 안내
     └── PathRunning(Idle의 하위 상태): path ACK accepted → 중립 20 Hz 유지, /api/path 폴링,
         스틱 입력·X·타임아웃·estop·owner 변경·ACK 거절 시 Idle로 복귀하고 사유 표시
```

규칙 구현 요점
- 중립 = `control_active:false, forward:0, turn:0`. AwaitNeutral에서는 데드맨을 쥐어도 Active로 가지 않고 HUD에 "중립 확인 중" 표시.
- Active 진입은 "RT ≥ 0.5 **그리고** Idle 확인됨"일 때만. 재진입마다 로봇이 `neutral_required`를 돌려주면 자동으로 AwaitNeutral로 돌아간다(사용자는 RT를 놓고 다시 쥐면 된다는 안내 표시).
- `out_of_order` → 시퀀스를 마지막 ACK+1로 재동기화. 3회 연속이면 `disconnect` 후 새 UUID로 재연결.
- 비상정지 해제는 터치 길게 누르기 1 s 뒤 `clear_emergency` → 중립 재확인까지 자동. 해제 뒤 데드맨이 이미 쥐어져 있으면 **놓았다가 다시 쥐어야** Active 허용(재프레스 규칙).
- 데드맨 해제 조건(모두 중립 송신): RT < 0.5, 패드 연결 끊김(XInput ERROR_DEVICE_NOT_CONNECTED), 창 포커스 상실(WM_ACTIVATE/WM_KILLFOCUS), 최소화, 설정 화면 진입. 앱 크래시는 로봇 watchdog 0.4 s가 뒷받침. 추가로 `SetUnhandledExceptionFilter`에서 `disconnect` 1회 송신을 시도(보장은 아니며 M7).
- 송신은 control 스레드 한 곳에서만. ACK 수신 2.0 s 없음 → "응답 없음" 표시, 소켓 재생성, UUID 유지(소유권 lease는 이미 만료됐을 것이므로 중립 확인부터).

### 3.4 에러 처리와 재연결

| 경로 | 감지 | 대응 |
|---|---|---|
| UDP v2 | ACK 2 s 부재 | 상태 "응답 없음", 럼블 2회, 중립 계속 송신 |
| rosbridge | 기존 자동 재연결(2 s) | 구독·광고 자동 복구(이미 구현), 서비스 호출 중이면 실패 콜백 → 토스트 |
| MJPEG | 기존 2 s 재연결 | 마지막 프레임 회색 처리 + "N초 전 프레임" 표시 |
| HTTP api | 타임아웃 1.5 s | 값 유지, 나이 표시, 3회 실패 시 폴링 간격 2배 |
| 패드 | XInput 오류 | 중립, 큰 안내 "컨트롤러 없음" |
| 로봇 발견 실패 | 1.5 s 응답 없음 | 수동 IP 안내, 같은 서브넷 확인 문구 |

### 3.5 크로스플랫폼 경계

- Mac(clang++, POSIX 소켓)에서 빌드·테스트: `core/` 전체, `cli/console`, `tests/`. 모의 로봇과 통합 테스트도 Mac에서 돈다.
- Windows 전용: `win/`(Win32, D3D11, XInput, Sensor API, 폰트). ImGui 백엔드도 여기만.
- `sock.hpp` 셔임이 유일한 OS 분기점이 되게 유지(`#ifdef _WIN32`는 core에서는 sock에만).

## 4. 빌드·실행

### 4.1 Ally(Windows, 폴더 안 GCC)

- 도구: `%USERPROFILE%\puppypi_heukgalli\toolchain\mingw64\bin\g++.exe`(WinLibs GCC 16.2 UCRT) + 같은 폴더의 `mingw32-make.exe`. 시스템 설치 없음, PATH 변경은 `build.ps1` 프로세스 안에서만.
- **CMake 추가하지 않음**: 설치가 필요하고(원칙 위반), 타깃이 두 개(앱·테스트)뿐이다. `Makefile` 하나로 Mac/Windows를 `UNAME` 분기로 처리하고, `build.ps1`은 PATH 설정 후 `mingw32-make`를 부르는 10줄 래퍼. **[결정]**
- 링크: `-static -static-libgcc -static-libstdc++ -lws2_32 -ld3d11 -ld3dcompiler -ldxgi -lxinput1_4 -lsensorsapi -lportabledeviceguids -lole32 -loleaut32 -luuid -luser32 -lgdi32 -ldwmapi -mwindows`. 결과물 단일 `puppypi_heukgalli.exe`(의존 DLL 없음 → 포트폴리오 배포 쉬움).
- ImGui 4개 .cpp는 `-O2`로 한 번만 컴파일되도록 오브젝트 분리. 디버그는 `make DEBUG=1`.
- 실행: `.\build.ps1 run`. 설정은 exe 옆 `config.json`. 로그는 `logs/날짜.txt`.
- Mac → Ally 동기화: 로컬 git + `scp`/`rsync`(SSH 이미 가능). Windows 쪽에서는 `git pull`이 아니라 rsync 받기(Ally에 git 미설치 가정 **[결정: Ally에 git 설치?]**).

### 4.2 Mac(코어 테스트)

- `make test` → `tests/*.cpp` 빌드·실행. `make console` → `cli/console`.
- `python3 tools/mock_robot.py` 띄운 뒤 `./build/console --host 127.0.0.1 --script tests/it_drive.txt`로 통합 테스트.

## 5. 테스트 전략

### 5.1 단위 테스트(Mac·Windows 공통, 자체 미니 assert, 외부 프레임워크 없음)

| 대상 | 케이스 |
|---|---|
| json | 파서 왕복, 숫자 정수/실수, 이스케이프, 2048 B 초과 거부, 깊이 제한 |
| v2 패킷 | `command/path/event` 직렬화가 정확히 허용 필드만 포함, 범위 클램프, path 1–20점 |
| ACK 파서 | 8필드 엄격 검증, 모르는 reason/state는 unknown으로 보존 |
| session | 상태 전이 표 전체: 중립 확인, neutral_required 재프레스, timeout, not_owner, estop/clear, out_of_order 재동기화, PathRunning 취소 사유 7종 |
| path_tools | RDP ε=0.08 m, 최소 간격 0.15 m, 20점 상한, 1점 경로, 역변환 왕복 u/v↔x/y(`/api/map` 예제값) |
| discovery | nonce 불일치 거부, URL 디코드, 포트·프로토콜 필드 |
| 입력 커브 | 데드존·지수·정밀 모드·트리거 스케일 단조성 |

### 5.2 통합 테스트(모의 로봇)

- **Python 모의 로봇을 테스트 픽스처로 유지한다(권장).** 이유: (1) 앱 코드와 독립된 구현이어야 오라클 역할을 한다. C++로 다시 쓰면 앱과 같은 가정을 공유해 버그를 서로 숨길 위험이 크다. (2) 포트폴리오의 "C++" 주장은 제품(앱)에 대한 것이고, 테스트 도구의 언어는 README에 명시하면 충분하다. (3) GitHub Actions에 Python이 기본 포함되어 CI 비용이 0이다. (4) C++ 모의 로봇은 하루가 더 든다(YAGNI).
- 단, **팀 로봇 코드를 import하는 현재 방식은 저장소에 넣지 않는다**(팀 코드 미동봉 원칙). `tools/mock_robot.py`는 문서화된 규칙(소유권 lease 1.0 s, watchdog 0.4 s, 중립 확인, estop 래치, 경로 12.5 cm/s·0.14 rad/s 운동학, 도착 0.10 m)을 **스펙 기준으로 재구현**한 독립 파일로 만든다. 팀 코드 래핑판은 로컬 비공개 검증용(`tools/private/`, .gitignore)으로만 둔다. **[결정]**
- 모의 로봇 범위: UDP 5001/5002 + 최소 rosbridge(서비스 응답, status/scan 가짜 토픽) + HTTP 8080(정지 JPEG 반복)·8090(`/api/*` 가짜 값). 시나리오 플래그: `--drop 5%`, `--latency 40ms`, `--second-owner`, `--estop-at 3s`, `--fault`.
- 시나리오 스크립트(`tests/it_*.txt`): 연결→중립→주행→놓기→경로 전송→스틱 개입 취소→estop→해제→재프레스→disconnect. 콘솔 클라이언트가 기대 상태 시퀀스를 비교해 exit code로 보고.

### 5.3 사용자 실기 점검표(`docs/HW_TEST_CHECKLIST.md`, 사용자가 수행·기록)

실기 이동은 사용자가 한다. 각 항목에 날짜·결과·메모 칸을 둔다.
1. 같은 Wi-Fi 확인, 발견 응답 수신, 수동 IP 연결
2. 중립 확인 → HUD `idle`. RT 쥐고 스틱 전진 소폭 → 로봇 전진, RT 놓음 → 즉시 정지
3. 후진 속도 캡(5 cm/s 체감), 회전 방향 좌우 일치
4. 패드 USB? 아님(내장) → 앱 창 포커스 전환(Alt+Tab) 중 로봇 정지 확인
5. B 비상정지 → 로봇 정지·래치, 해제 길게 누르기 → 중립 재확인 → RT 재프레스 요구
6. rosbridge 비상정지/해제(F13) 동작과 v2 estop의 관계 관찰(해제 순서 기록)
7. go_home, 자세 프리셋 7개, 동작 그룹 3개 이상(wave, sit, stand) 실행 중 주행 차단 표시
8. 지도 저장/초기화, 지도 화면 로봇 위치가 실제와 일치
9. 경로 스케치 2–3점 짧은 경로 → rotate→drive→done. 스틱 개입 취소, X 취소
10. 카메라 fps·프레임 나이, 레이더 HUD 방향 일치(전방 벽이 위)
11. 통신 차단 시험: Wi-Fi 끄기 → 0.4 s 내 정지(로봇 watchdog), 앱 복구 시 중립 확인부터
12. 배터리 30분 연속 사용 시 발열·fps
13. (옵션) 기울임 조향 체감

### 5.4 CI(선택, 저렴할 때만)

- ubuntu-latest: `make test` + 모의 로봇 통합 테스트 1개(≈1분).
- windows-latest: `msys2/setup-msys2`로 mingw-w64 g++ 설치 후 앱 빌드만(실행 없음). 로컬 GCC 16.2와 버전이 달라 경고 차이는 허용. 비용이 커지면 Windows 잡은 뺀다. GitHub 공개 이후에만 의미 있음 **[결정]**.

## 6. 포트폴리오 산출물

- **README**(한국어 본문 + 영어 요약 상단 **[결정: 언어 비중]**): 한 줄 소개 → 30초 GIF → 왜 Ally X → 기능표(2.1 요약) → 아키텍처 그림 → 안전 모델(데드맨·watchdog·estop 3층) → 측정값(실측만) → 빌드 방법 → 테스트 → 배운 점 → 한계·범위 밖 → 라이선스·THIRD_PARTY.
- **아키텍처 다이어그램**: `docs/architecture.svg`(스레드·포트·데이터 흐름) + 상태 머신 그림. Mermaid 소스를 README에 두고 SVG 내보내기.
- **스크린샷/GIF/영상**: 주행 HUD(레이더 포함), 지도 스케치→로봇 추종, 로봇 화면, 로그 화면 RTT 히스토그램. 영상 60–90 s: Ally 화면 녹화(Xbox Game Bar 가능 여부 확인) + 외부 폰 촬영 합성. 실제 로봇 장면은 사용자가 촬영.
- **측정(실측만, `docs/MEASUREMENTS.md`에 날짜·조건·n·분포 기록)**:
  - ACK RTT: 송신→ACK 수신, 같은 시계(Ally 단조 시계). 중앙값·p95, n ≥ 1000, 모의 로봇과 실기 분리 기록.
  - 명령 주기 지터: 20 Hz 목표 대비 실제 간격 분포.
  - 카메라: 수신 fps, 프레임 나이(수신 시각→화면 표시 시각, Ally 내부). 네트워크 대역폭은 Windows 성능 카운터로 보조.
  - rosbridge 서비스 RTT(go_home 등).
  - UI 프레임 시간(120 Hz 유지 여부).
  - **측정하지 않는 것**: 스틱 입력→로봇 다리 움직임의 종단 지연. 로봇 시계와 동기화 근거가 없고 외부 고속 카메라가 필요하다. README에 "미측정"으로 명시. 미래 과제로 폰 240 fps 촬영 방법만 기록.
- **LICENSE**: MIT 권장(ImGui MIT, stb MIT/Unlicense와 호환, 포트폴리오 재사용 자유) **[결정]**. `THIRD_PARTY.md`에 ImGui 1.93.0, stb_image 2.32, (번들 시) 폰트 라이선스 기록. Hiwonder·팀 코드 미포함을 명시.
- **"배운 점" 후보**: 소유권·lease·watchdog이 있는 UDP 프로토콜 클라이언트의 상태 머신 설계, 데드맨과 포커스 상실 처리, WebSocket 프레이밍 직접 구현, ImGui 터치 UI의 엄지 도달 설계, MinGW 정적 링크 단일 exe, 측정의 정직한 경계(무엇을 측정할 수 없었는가).

## 7. 마일스톤(순서, 수락 기준)

| M | 내용 | 수락 기준 | 예정 |
|---|---|---|---|
| M0 | 코어 컴파일·단위 테스트(Mac) | `make test` 전부 통과. json/session/path_tools/discovery 테스트 존재 | 11-14 오전 |
| M1 | 스펙 기반 모의 로봇 + 콘솔 클라이언트 통합 | `it_drive` 시나리오 통과(연결→주행→경로→estop→해제→disconnect), RTT 출력 | 11-14 오전 |
| M2 | Windows 골격(Ally에서 빌드) | `build.ps1`로 단일 exe, 창 1920×1080, ImGui 한글 표시, 패드 값 화면 표시, 럼블 1회 | 11-14 오후 |
| M3 | 주행 MVP | 발견/수동 IP, 중립 확인, RT 데드맨 주행, B/터치 estop, 해제, 포커스 상실 중립, 카메라 표시, 상태띠, 럼블 패턴 4종. 모의 로봇으로 확인 | 11-14 저녁 |
| M4 | rosbridge 기능 | 로봇 화면(프리셋·동작 그룹·go_home), rosbridge estop/reset, 지도 저장/초기화, status/arbiter 띠, 레이더 HUD | 11-15 오전 |
| M5 | 지도·경로 스케치 | 지도 JPEG+`/api/map` 변환, 스케치→≤20점→전송, `/api/path` 진행 표시, 취소 규칙 | 11-15 오후 |
| M6 | 측정·문서·옵션 | RTT/지터/fps 로그 화면, 스냅샷, (가능하면) 기울임 조향, README 초안, 다이어그램, HW 점검표 | 11-15 저녁 |
| M7 | 실기 피드백 반영·마감 | 사용자 점검표 결과 반영, 스크린샷/GIF, THIRD_PARTY, 크래시 시 disconnect 시도, CI | 이후 |

M3까지가 MVP. M3 완료 시점에 사용자 실기 점검표 1–6번을 먼저 받고, 결과를 M4 이후에 반영한다. 보조 작업자(Claude)는 모의 로봇까지만 돌리고 실기 이동은 하지 않는다.

## 8. 리스크와 미확인

| 리스크 | 영향 | 완화 |
|---|---|---|
| Windows Sensor API로 가속도계가 안 보일 수 있음("Simple Device Orientation Sensor"만 노출되거나 권한 설정 필요) | 기울임 조향 불가 | 기본 꺼짐 옵션으로 격리. 시작 시 센서 열기 실패하면 설정에서 회색 처리. 포트폴리오 핵심 아님 |
| Armoury Crate 모드: 패드가 XInput이 아닌 데스크톱(마우스/키보드) 모드면 입력 없음. M1/M2 뒷버튼은 XInput에 안 잡힐 가능성 | 조작 불가 | 첫 화면에서 XInput 상태 표시와 "게임패드 모드로 전환" 안내. M1/M2는 보너스로만 |
| ImGui 터치: 멀티터치 미지원, 드래그 스크롤·핀치 없음, 작은 위젯 | 조작 실수 | 96 px 이상 버튼, 스케치는 단일 터치 드래그만, 확대는 −/+ 버튼. Win32 백엔드가 터치를 마우스로 받는지 M2에서 확인 |
| 한글 폰트 아틀라스 크기: 전체 한글 11,172자는 아틀라스가 커짐 | 메모리·시작 시간 | UI 문구에서 쓴 글자만 `ImFontGlyphRangesBuilder`로 수집 + KS X 1001 2,350자. 시스템 `malgun.ttf`를 실행 시 로드(재배포 안 함) 또는 OFL 폰트 번들 **[결정]** |
| 카메라 15–40 Mbps MJPEG: 2.4 GHz 혼잡 시 끊김 | 화면 멈춤, 조작 판단 지연 | 프레임 나이 표시, 5 GHz 권장 문구, 대안으로 `type=mjpeg&width=640&quality=60`(Pi 재인코딩 부담 → 실기 확인 후 **[결정]**) |
| 로봇 네트워크 경로 없음(흑갈리와 로봇이 서로 다른 Wi-Fi) | 실기 불가 | 모의 로봇으로 개발 진행. 같은 AP 접속은 사용자 작업. 앱은 서브넷 다르면 경고 |
| v2 estop과 rosbridge estop의 상호작용 미확인(두 래치 해제 순서) | 해제 혼란 | 모의 로봇은 문서 규칙만 구현. 실기 점검표 6번으로 확인 뒤 UI 문구 확정 |
| MinGW 정적 링크에서 Sensor API COM 헤더 호환 문제 가능 | 빌드 실패 | 센서 모듈을 별 오브젝트로 두고 `-DNO_TILT`로 빼고 빌드 가능하게 |
| 20 Hz 송신 스레드가 Windows 타이머 해상도(기본 15.6 ms)에 흔들림 | 지터 | `timeBeginPeriod(1)` 또는 waitable timer 고해상도, 지터를 실측해 기록 |
| 모의 로봇과 실기 차이(ACK 지연, 경로 운동학) | 통합 테스트 과신 | README에 "모의 로봇 통과 ≠ 실기 검증" 명시, 실기 결과는 점검표 기록으로만 주장 |

## 9. 사용자 결정 목록(요약)

1. 비상정지 B 버튼이 v2 `emergency_stop`과 rosbridge `/puppy_control/emergency_stop`을 **동시에** 보낼지, v2만 보낼지.
2. 주행 스틱: 기본 "LS 전후 + RS 회전"으로 갈지, 단일 스틱 모드도 넣을지.
3. Python 모의 로봇 유지 + 스펙 기반 재구현(권장) vs C++ 모의 로봇. 팀 코드 래핑판은 비공개 폴더로 분리하는 데 동의하는지.
4. 빌드: Makefile + build.ps1만(권장) vs CMake 추가.
5. Ally에 git 설치 여부(아니면 rsync로 동기화).
6. 한글 폰트: 시스템 `malgun.ttf` 실행 시 로드 vs OFL 폰트 번들.
7. LICENSE: MIT(권장) vs 다른 것.
8. README 언어 비중: 한국어 본문 + 영어 요약 vs 완전 이중 언어.
9. 기울임 조향을 MVP 이후 옵션(RB 홀드 중만)으로 둘지, 아예 빼는지.
10. 카메라 대역폭 문제가 실기에서 보이면 `type=mjpeg` 저해상도 재인코딩을 쓸지(Pi CPU 부담).
11. 동작 그룹 중 "확인 필요"로 표시할 항목(예: jump, up_stairs_*, boxing 계열).
12. CI(GitHub Actions) 추가 시점과 GitHub 공개 여부.
13. 저장소·앱 이름 최종(현재 `puppypi_heukgalli`).
