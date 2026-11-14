# puppypi_heukgalli UI 재설계 계획 v1

작성: 2026-11-14 | 상태: **기록용**(일부 구현 뒤 `DESIGN_PLAN_v2.md` 로 대체됨) | 대상: `src/win/ui.cpp`, `src/win/app_main.cpp` (Dear ImGui 1.93.0, Win32 + D3D11)
화면 기준: ROG Ally X 7" 1920×1080, DPI 배율 1.5 → **논리 해상도 1280×720**. 아래 치수는 모두 논리 px(1.0 배율)이며 괄호 안이 실제 px.
근거 스크린샷: `docs/screenshots/{drive,map,robot,log}.png` (2026-11-14, mock 로봇).

---

## 1. 진단: 왜 지금 화면이 "밤티"로 보이는가

| # | 문제 | 어디서 보이나 | 원인(코드) |
|---|---|---|---|
| D1 | **기본 ImGui 룩** — 평평한 단색 사각 버튼, 동일 회색, 라운드만 10 | 모든 화면 | `StyleColorsDark()` + 버튼 3색만 덮어씀(`app_main.cpp:233-246`) |
| D2 | **위계 없음** — 내비 버튼, 기능 버튼, 비상정지 해제 버튼이 같은 모양·크기. 선택된 탭도 "조금 더 파란 사각형"뿐 | 왼쪽·오른쪽 레일 | `big_button()` 하나로 모든 버튼을 그림(`ui.cpp:357`) |
| D3 | **텍스트 과다·아이콘 0개** — "지도·경로", "연결·설정", "화면: +레이더", "패킷 기록 켜기" 등 글자 버튼만. 한눈에 구분되지 않음 | 레일, 상태띠 | 아이콘 폰트 미병합(seguisym만) |
| D4 | **상태띠가 빽빽하고 색이 뒤섞임** — 노랑 앱 이름, 초록 "패드", 흰 IP, 파랑 칩, 초록 RTT, 초록 ROS, 흰 카메라가 한 줄에 균일 간격. 중요한 "주행 상태"가 두드러지지 않음 | 상단 64 px 띠 | `draw_status_bar()`가 텍스트·칩을 좌→우로 나열만 |
| D5 | **저대비 보조 텍스트** — `TextDisabled`(회색 50 %)로 "자세 (D-pad ▲▼)", "속도" 등 실제 조작 힌트를 흐리게 표시 | 왼쪽 레일 하단, 로봇·로그 화면 제목 | ImGui 기본 Disabled 색 |
| D6 | **이벤트 띠 소음** — 하단 96 px을 로그 3줄이 항상 차지. 운전 중에는 거의 읽지 않는 정보가 화면의 13 %를 씀 | 모든 화면 하단 | `draw_events()` tail(3) |
| D7 | **주행 HUD가 배경과 분리** — 전후/회전 막대 패널이 검은 반투명 사각형으로 카메라 위에 "붙어" 있고, 데드맨 문구는 회색 작은 글자. 운전 가능 여부가 한눈에 안 보임 | drive.png 좌하단 | `draw_drive()` 막대·문구 수동 배치 |
| D8 | **레이더 라벨 겹침** — "LiDAR 3 m" 텍스트가 스캔 점 위에 겹치고, 원 바깥 영역 경계가 없음 | drive.png 우하단 | `draw_radar()` 라벨 위치 고정 |
| D9 | **로봇 화면 격자 잘림·스크롤바 노출** — 4열 격자의 4번째 열이 스크롤바에 가려짐("내려보기 10°"), 24개 동작 버튼은 스크롤해야 보임. 주의(노랑) 버튼은 "갈색 사각형" | robot.png | `draw_robot()`에서 폭 계산에 ScrollbarSize 미반영 |
| D10 | **로그 화면이 디버그 덤프** — 중재기 JSON 원문, 긴 한 줄 통계, 흰 선 하나의 PlotLines | log.png | `ImGui::Text`·`PlotLines` 기본 |
| D11 | **지도 화면 배경 불일치** — 중앙 영역만 밝은 회색(#282830), 좌우 레일은 어두운 남색. 경고 문구가 노란 작은 글자로 지도 위에 떠 있음 | map.png | `draw_map()` 배경색 하드코딩 |
| D12 | **비상정지 버튼은 눈에 띄지만 "버튼처럼" 보이지 않음** — 평면 빨강 사각형 + 두 줄 글자. 해제 진행도는 바닥의 얇은 초록 띠 | 우하단 | `estop_button()` |

잘 된 점(유지): 비상정지가 항상 우하단에 크게 있음, 좌우 레일 + 중앙 보기 전용이라는 엄지 도달 구조, 상태띠가 비상정지 시 진홍으로 바뀜, 카메라 원본 비율 유지, 버튼 높이 ≥ 50 논리 px(75 px).

---

## 2. 시각 방향: "검은 야전 리모컨(Field Remote)"

**한 문장**: 검은 흑갈리 본체에서 이어지는 무광 흑색 패널 위에, 카메라·지도 같은 "실제 세계"를 가장 밝게 두고, UI는 얇은 선·작은 칩·한 가지 강조색으로만 말한다. 장식보다 **읽는 시간(글랜스)** 을 줄이는 데 모든 선택을 맞춘다.

무드 키워드: 무광 흑색 섀시, 미션 컨트롤 텔레메트리, 경고는 색 + 모양 + 글자 세 겹, 둥글지만 과하지 않음(라운드 8-12), 네온 없음.

참고 제품에서 가져올 원칙(브랜딩·아트워크는 쓰지 않음, 원칙만):

| 출처 | 가져올 원칙 | 적용 |
|---|---|---|
| [QGroundControl Fly View](https://docs.qgroundcontrol.com/master/en/qgc-user-guide/fly_view/fly_view.html) | 상단 툴바 = 상태 인디케이터, 영상/지도 전환기는 좌하단 소창, 계기판은 값만 큼직하게 | 상태띠를 "인디케이터 묶음"으로, 레이더를 PIP처럼 테두리 있는 소창으로 |
| [Auterion Mission Control Fly View](https://docs.auterion.com/vehicle-operation/auterion-mission-control/ui-breakdown/fly/fly-view-ui-overview) | 현재 상태에서 **유효한 동작만 보이고 나머지는 숨기거나 회색**, 위험 동작은 **홀드 확인**, 알림은 "원인 + 권장 조치" 팝업, 상태 아이콘을 탭하면 상세 | 경로 보내기 비활성 사유 표시, 큰 동작 홀드, 토스트에 조치 문구 |
| [Mission Planner HUD](https://ardupilot.org/planner/docs/mission-planner-flight-data.html) | HUD 한 구역에 자세·링크·모드가 모여 있어 시선 이동이 짧음 | 주행 상태 캡슐에 데드맨·상태·사유를 한 덩어리로 |
| [DJI Fly 가이드](https://www.pix-pro.com/dji-fly-guide-part1) (3자 요약) | 좌상단 상태바를 탭하면 점검표·경고, 텔레메트리는 하단 한 줄, 촬영 조작은 우측 | 텔레메트리 칩 좌상단, 조작은 우측 레일 유지 |
| [Boston Dynamics Spot Tablet](https://bostondynamics.com/products/extras/spot-tablet/) / [Pro](https://bostondynamics.com/products/extras/spot-tablet-controller-pro/) | 8" 터치 + 물리 스틱, **STOP은 물리 버튼으로 모터 전원 차단**([Spot 사용자 가이드 PDF](https://www.generationrobots.com/media/spot-boston-dynamics/spot-user-guide-r2.0-va.pdf)), 설계사 Rightpoint는 모바일 게임 UI에서 터치 조작을 가져옴([IoT World Today](https://iotworldtoday.com/robotics/boston-dynamics-spot-the-design-behind-the-robot-dog)) | 화면 비상정지는 B 버튼의 보조라는 점을 UI 문구에 유지 |
| [Unitree Go2 운용 가이드(멜버른대)](https://ms-kb.msd.unimelb.edu.au/next-lab/robotics/robotic-dog/operation-setup/operation-basics) | 홈 화면 좌상단 **초록 연결 상태**, 안전 명령은 물리 컨트롤러가 빠르다 | 연결 상태를 좌상단 첫 칩으로 고정 |
| [Clearpath OutdoorNav Manual Mode](https://docs.clearpathrobotics.com/docs_outdoornav_user_manual/web_user_interface/ui_manual_mode), [Assisted Teleop](https://docs.clearpathrobotics.com/docs_outdoornav_user_manual/features/assisted_teleoperation) | 조이스틱 **lockout 토글**, 장애물 근접 경고, 패드는 창 포커스 중에만 동작 | 포커스 상실 = 중립을 화면에 명시("포커스 없음" 상태) |
| [ANYbotics ANYmal Workforce](https://www.anybotics.com/robotics/robot-capabilities/) | "safety-centric UX", 비전문가용 | 위험 동작의 시각 언어를 통일(주황 테두리 + 경고 아이콘) |
| [Foxglove panels](https://docs.foxglove.dev/docs/visualization/panels/introduction), [ROS 2 문서](https://docs.ros.org/en/jazzy/How-To-Guides/Visualizing-ROS-2-Data-With-Foxglove-Studio.html) | 어두운 바탕 위 패널 단위 구성, 패널마다 제목줄 | 로그 화면을 통계 타일 + 플롯 패널로 |
| [ROS-Mobile 논문(arXiv 2011.02781)](https://arxiv.org/abs/2011.02781) | 위젯 단위(조이스틱·배터리) 조립 | `widgets.hpp`의 컴포넌트 분리 |
| [Microsoft Designing for Xbox and TV](https://learn.microsoft.com/en-us/windows/apps/design/devices/designing-for-tv) | 본문 15 epx·보조 12 epx 이상, 대화형 요소 32 epx 이상, 가장자리 5 %는 비필수 요소만, 어두운 테마 기본, 극단 채도 피함, 포커스 시각을 크게 | 타이포 스케일·안전 여백·포커스 링 |
| [Gamepad and remote interactions](https://learn.microsoft.com/windows/uwp/design/input/gamepad-and-remote-interactions) | 포커스 시각은 먼 거리에서도 보이게, 버튼 글리프는 Segoe 계열 심볼 | 패드 글리프 표준화 |
| Steam Deck 커뮤니티 정리([steamcommunity](https://steamcommunity.com/groups/SteamClientBeta/discussions/0/3710433479218773768), [Steamworks FAQ](https://partner.steamgames.com/doc/steamdeck/faq)) | 컨트롤러 종류에 맞는 버튼 프롬프트, 단일 터치 제약 | 글리프를 Xbox 배치로 고정, 멀티터치 가정 안 함 |
| Steam Deck 산업디자인([Ben Collette](https://bencollette.myportfolio.com/steamdeck)) | "화면 콘텐츠가 왕, 기기는 조력자", 고대비 가독성, 하드웨어 색과 UI 색의 일관성 | 검은 섀시와 이어지는 #0B0D10 바탕 |
| Nintendo Switch UI 리뷰([Medium](https://medium.com/@dli_li/nintendo-switch-ui-design-1eb1742515db), [Nintendo Life](https://www.nintendolife.com/news/2017/01/heres_your_first_look_at_the_nintendo_switch_user_interface)) | 평면 아이콘, 넉넉한 여백, 명확한 위계와 가독성 | 아이콘 + 짧은 라벨, 여백 8의 배수 |
| 게임 HUD 원칙([pixune](https://pixune.com/blog/game-hud-design/), [wayline](https://www.wayline.io/blog/your-ui-is-killing-immersion-fix-it)) | 상시 요소는 최소, 가끔 필요한 것은 페이드 인/아웃, 배경 대비 확보 | 이벤트 띠 1줄 + 토스트, HUD 텍스트 그림자 |
| 차량 HMI(NHTSA 2초 글랜스 요약: [사례](https://portfolio-omega-sable-60.vercel.app/car-hmi), [AAA 리뷰](https://aaafoundation.org/research/human-machine-interfaces-and-vehicle-automation-a-review-of-the-literature-and-recommendations-for-system-design-feedback-and-alerts/)) | 한 번에 2초 안에 읽히는 양, 지속 상태는 시각으로 | 주행 상태 캡슐은 3단어 이내 |
| 비상정지 규격 요약([Control Design](https://www.controldesign.com/safety/safety-components/article/21526010/standards-guide-the-use-of-estops), [EAO](https://www.newark.com/pdfs/techarticles/eao/EAO_Emergency_Stops-What_Designers_Need_to_Know.pdf)) | 빨간 버섯머리 + 노란 배경이 비상정지의 시각 언어, HMI 소프트 버튼은 1차 비상정지가 아님 | 비상정지 버튼에 노란 링을 넣어 "비상정지" 기호성 확보, README에 보조 수단임을 명시 |
| 3단 인에이블 장치([Universal Robots 3PE](https://www.universal-robots.com/manuals/EN/HTML/SW5_19/Content/prod-usr-man/complianceUR30/3petp_UR30/3petp-UR30-description.htm)) | "쥐고 있어야 움직임"은 로봇 업계 표준 개념 | RT 데드맨 상태를 인에이블 램프처럼 |
| Dear ImGui 갤러리 [Part 19](https://github.com/ocornut/imgui/issues/7503), [Part 20](https://github.com/ocornut/imgui/issues/7959), [Part 21](https://github.com/ocornut/imgui/issues/8333), [창 그림자 이슈 #1329](https://github.com/ocornut/imgui/issues/1329) | 기본 룩을 벗어난 앱은 공통적으로 (a) 자체 폰트 (b) 아이콘 폰트 병합 (c) 드로우 리스트로 직접 그린 위젯 (d) 반투명 라운드 사각형 겹치기로 그림자 흉내 | §5·§6 전체 |
| [ImGui FONTS.md (1.92+)](https://github.com/ocornut/imgui/blob/master/docs/FONTS.md) | 크기 없는 로드, `PushFont(nullptr, size)`, MergeMode, 글리프 범위 불필요, `GlyphExcludeRanges` | §6.1 |

---

## 3. 디자인 토큰

### 3.1 색 (WCAG 대비는 `python3` 상대휘도 계산으로 검증, 값은 아래 표)

| 토큰 | HEX | 용도 | 검증된 대비 |
|---|---|---|---|
| `bg0` | `#0B0D10` | 창 바탕(섀시와 이어짐), 카메라 레터박스 | — |
| `bg1` | `#13161B` | 레일·상태띠 패널 | — |
| `bg2` | `#1B2027` | 카드, 버튼 기본면 | — |
| `bg3` | `#242A33` | 버튼 hover, 슬라이더 트랙 | — |
| `line` | `#2E3640` | 1 px 구분선, 카드 외곽선 | — |
| `text1` | `#F2F4F7` | 본문·값 | bg0 17.7 : 1, bg3 13.1 : 1 |
| `text2` | `#A9B1BD` | 라벨·단위 | bg1 8.4 : 1, bg3 6.7 : 1 |
| `text3` | `#6F7987` | 힌트(비필수, 14 px 이상에서만) | bg1 4.1 : 1, bg2 3.7 : 1 (큰 글자/비필수 기준 3 : 1 충족, 본문 기준 4.5 : 1 미달 → 본문에 쓰지 않음) |
| `accent` | `#FF6B35` | 선택된 탭, 활성 토글, 포커스 링, 진행 링 | bg1 6.4 : 1. **accent 위 글자는 `ink`** (ink 6.9 : 1, 흰색은 2.8 : 1로 미달) |
| `ok` | `#3DD68C` | 연결됨·주행 중·수락 | bg1 9.7 : 1, 위 글자 ink 10.4 : 1 |
| `warn` | `#F5A524` | 중립 확인 중·큰 동작·주의 | bg1 8.9 : 1, 위 글자 ink 9.5 : 1 |
| `danger` | `#E5484D` | 비상정지·끊김·거절 | bg1 4.6 : 1, 위 글자 ink 5.0 : 1, 흰 글자 3.9 : 1(24 px 이상 굵은 글자만) |
| `danger_deep` | `#B3261E` | 비상정지 버튼 하단 그라데이션, 비상정지 중 상태띠 바탕 | 흰 글자 6.5 : 1 |
| `info` | `#5B9CFF` | 대기(idle)·정보 | bg1 6.6 : 1, 위 글자 ink 7.1 : 1 |
| `lidar` | `#19C6D1` | 레이더 점·격자(초록 ok와 구분되는 청록) | bg0 9.3 : 1 |
| `ink` | `#0B0D10` | 밝은 칩 위 글자 | — |
| `scrim` | `#0B0D10` α 0.55 | 카메라 위 HUD 패널 바탕 | — |

규칙: 의미색(ok/warn/danger/info)은 **항상 아이콘 또는 글자와 함께** 쓴다(색만으로 구분하지 않음). 한 화면에 accent는 한 요소군(현재 탭 + 포커스)에만. 네온·발광 효과 없음.

### 3.2 타이포그래피 (1.0 배율 논리 px, 실제는 ×1.5)

| 역할 | 폰트 | 크기 | 굵기 |
|---|---|---|---|
| 본문 `body` | 본문 폰트 | 18 (27) | Regular |
| 라벨 `label` | 본문 폰트 | 15 (22.5) | Regular, text2 |
| 힌트 `hint` | 본문 폰트 | 14 (21) | Regular, text3 |
| 제목 `title` | 본문 폰트 | 22 (33) | Bold |
| 버튼 `button` | 본문 폰트 | 17 (25.5) | SemiBold/Bold |
| 큰 값 `num_lg` | 숫자 폰트 | 28 (42) | Regular |
| 작은 값 `num_sm` | 숫자 폰트 | 16 (24) | Regular |
| 비상정지 라벨 | 본문 폰트 | 26 (39) | Bold |
| 아이콘 | 아이콘 폰트 | 20 / 28 (30 / 42) | — |

폰트 선택(§6.1에서 상세):
- **기본 경로(번들 0 byte)**: 본문 = 맑은 고딕 `malgun.ttf` + `malgunbd.ttf`(Bold), 숫자 = `CascadiaMono.ttf` 있으면 그것, 없으면 `consola.ttf`, 아이콘 = **Segoe Fluent Icons** `SegoeIcons.ttf`(Windows 11 기본, [MS 문서](https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-fluent-icons-font)) 없으면 `segmdl2.ttf`.
- **권장 경로(번들 약 2.5-3 MB, 사용자 결정)**: 본문 = **Pretendard** Regular + Bold(정적 TTF, OFL 1.1, 저작권자 길형진, [README](https://cdn.jsdelivr.net/npm/pretendard@1.3.9/docs/en/README.md)). 맑은 고딕보다 자간·숫자·라틴 혼용이 고르고 Switch/Deck 류 UI 톤에 맞는다. 가변(Variable) 판은 stb_truetype이 축을 못 읽으므로 정적 판을 쓴다.
- 아이콘 대안(번들): Material Symbols Rounded(Apache 2.0, [Google 가이드](https://developers.google.com/fonts/docs/material_symbols)) 또는 Lucide(ISC, [lucide-static](https://npmjs.com/package/lucide-static)). Windows 10 지원이 필요하거나 Segoe Fluent 글리프가 부족할 때만.
- 패드 글리프(Ⓐ Ⓑ Ⓧ Ⓨ, LB/RB, LT/RT, D-pad)는 **폰트에 의존하지 않고 드로우 리스트로 직접 그린다**(§5.2). Segoe Fluent Icons에도 ButtonA `U+F093`, ButtonB `F094`, ButtonY `F095`, ButtonX `F096`, ButtonMenu `EDE3`, LeftStick `F108`, RightStick `F109`, TriggerLeft `F10A`, TriggerRight `F10B`, BumperLeft `F10C`, BumperRight `F10D`, Dpad `F10E`가 있으나 Windows 10/다른 기기에서 빠질 수 있다.

### 3.3 간격·형태·선

- 간격 그리드 **8** (4의 보조). 패널 안쪽 여백 16, 요소 간 12, 칩 안쪽 가로 10 세로 6.
- 모서리: 카드·패널 12, 버튼 10, 칩 8(완전 둥근 캡슐은 높이/2), 비상정지 20.
- 선: 구분선 1, 카드 외곽 1(`line`), 포커스 링 3(`accent`), 게이지 트랙 6, 진행 링 6.
- 터치 타깃: 최소 **48×48 논리(72 px)** — Material 48 dp·Apple 44 pt·WCAG 2.5.8 24 px 모두 충족([정리](https://www.designsystemscollective.com/a-designers-field-guide-to-wcag-ios-and-android-rules-31ba5d01d46a)). 레일 버튼 64 높이(96 px), 격자 버튼 56(84 px), 숫자 키패드 64(96 px), 비상정지 120×120(180 px).
- 가장자리 안전 여백: 화면 외곽 12(18 px) 안에는 필수 조작 요소를 두지 않음(TV 가이드 5 %를 7" 기준으로 완화).

### 3.4 입체감(ImGui에는 블러가 없으므로 흉내)

- **카드 떠 있음**: 카드 아래에 `AddRectFilled(p0+(0,2), p1+(0,2), IM_COL32(0,0,0,90), r)` 1장 + `(0,6)` 오프셋에 α 40 한 장. 두 장이면 충분하고 120 Hz에 부담 없음([#1329의 "반투명 라운드 사각형 겹치기"](https://github.com/ocornut/imgui/issues/1329)).
- **버튼 면 그라데이션**: `AddRectFilled(..., rounding)` 뒤 `ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtx0, vtx1, p0, ImVec2(p0.x, p1.y), top, bottom)`(imgui_internal.h). 라운드를 유지하면서 세로 그라데이션. `AddRectFilledMultiColor`는 라운드가 없으므로 라운드가 필요 없는 띠(상태띠 바탕)에만.
- **안쪽 하이라이트**: 윗변 1 px `IM_COL32(255,255,255,18)` 선.
- **HUD 텍스트 그림자**: 같은 글자를 `(1,1)` 오프셋 `IM_COL32(0,0,0,160)`으로 먼저 찍고 본문을 찍는다. 카메라 위 글자는 모두 이 규칙.

### 3.5 모션

| 이벤트 | 시간 | 곡선 | 구현 |
|---|---|---|---|
| 탭 전환 하이라이트 이동 | 160 ms | ease-out(1−(1−t)²) | 레일 하이라이트 y를 보간 |
| 버튼 눌림 | 즉시 어두워짐 + 80 ms 복귀 | — | `IsItemActive`면 면색 −8 % |
| 토스트 | 120 ms 등장(아래서 8 px 올라옴), 3 s 유지, 200 ms 페이드 | ease-out | α·y 보간 |
| 비상정지 해제 홀드 링 | 1.0 s 선형(기존 동작 유지) | linear | PathArcTo |
| 데드맨 램프 ON | 즉시(안전 정보는 지연 금지) | — | — |
| 데드맨 램프 OFF | 즉시, 잔상 없음 | — | — |
| 연결 끊김 | 상태띠 즉시 danger, 0.8 s 주기 2회 깜빡 후 고정 | — | `fmod(now,0.8)` |
| 레이더 스윕 | **없음**(장식 애니메이션 금지, 배터리·주의 분산) | — | — |

모든 보간은 `now` 기반(프레임 독립). 120 Hz와 60 Hz에서 같은 시간.

---

## 4. 화면 레이아웃 (논리 1280×720)

공통 뼈대 — 지금의 상단 64 / 하단 96 / 레일 168 을 **상단 44 / 하단 28 / 왼쪽 레일 80 / 오른쪽 레일 128** 로 바꾼다. 중앙은 1072×648 (16:9 카메라 1072×603 수용).

```
0        80                                              1152     1280
┌────────┬────────────────────────────────────────────────┬─────────┐ 0
│ 상태띠: [●연결 192.168.x.x] [주행상태 캡슐] [RTT 18ms] [ROS] [CAM 10] [패드]   │ 44
├────────┼────────────────────────────────────────────────┼─────────┤
│ 나브   │                                                │ 컨텍스트│
│ 레일   │                 중앙 (화면별)                   │ 레일    │
│ 80     │                 1072 × 648                      │ 128     │
│ 아이콘 │                                                │ 아이콘+ │
│ +라벨  │                                                │ 라벨    │
│        │                                                │ 버튼    │
│ ─────  │                                                │         │
│ 자세   │                                                │ ┌─────┐ │
│ 동작   │                                                │ │비상 │ │
│ 속도   │                                                │ │정지 │ │
│        │                                                │ └─────┘ │ 692
├────────┴────────────────────────────────────────────────┴─────────┤
│ 이벤트 1줄: 09:37:06 로봇 상태 idle                      [∧ 펼치기] │ 720
└───────────────────────────────────────────────────────────────────┘
```

- 왼쪽 레일 80: 아이콘 28 + 라벨 14 세로 적층, 버튼 64×64, 선택 탭은 `accent` 세로 바 4 px + 아이콘·라벨 text1, 비선택은 text2. 레일 하단에 "자세/동작/속도" 3개 **칩**(글자 대신 값만, 라벨은 hint).
- 오른쪽 레일 128: 아이콘 버튼 128×56(아이콘 좌, 라벨 우, 패드 글리프 우측 끝). 맨 아래 비상정지 120×120 중앙 정렬, 위에 12 여백.
- 하단 28: 최신 이벤트 1줄(text2) + 우측 "펼치기" 탭. 펼치면 중앙 위로 160 높이 패널이 올라와 8줄 표시(로그 화면 전용 아님). 운전 중(`held_`)에는 자동으로 접힘.
- 상태띠 44: 좌→우 = 연결 칩(●+호스트) · **주행 상태 캡슐(가장 크고 유일한 채움색)** · RTT/유실(숫자 폰트) · ROS 아이콘 · 카메라 아이콘+fps · 패드 아이콘. 토스트는 상태띠가 아니라 중앙 상단에 별도(§5.6).

### 4.1 주행(Drive)

```
┌────────┬────────────────────────────────────────────────┬─────────┐
│ ▣ 주행 │ ╭RTT 18ms╮ ╭CAM 10fps · 0.1s╮ ╭SCAN 0.2s╮        │ ▦ 레이어 Ⓨ│
│ ▦ 지도 │                                                │ ◉ 사진 RB│
│ ✣ 로봇 │                                                │ ⌂ 서기 Ⓐ│
│ ≣ 로그 │              카메라 1072×603 (16:9)            │         │
│ ⚙ 연결 │                                                │         │
│        │                                   ┌──────────┐ │         │
│ 자세   │                                   │ 레이더   │ │         │
│ [서기] │                                   │ 3 m  PIP │ │         │
│ 동작   │                                   │ 320×320  │ │ ┌─────┐ │
│ [손흔들]│  전후 ◀━━━━━┿━━━━━▶  회전 ◀━━━┿━━━▶  └──────────┘ │ │ ⬢   │ │
│ 속도   │  ┌──────────────────────────────┐                │ │비상 │ │
│ [60%]  │  │ ● 주행 중 · 패드 · RT 쥠     │                │ │정지 │ │
│        │  └──────────────────────────────┘                │ │ Ⓑ   │ │
├────────┴────────────────────────────────────────────────┴─────────┤
│ 09:37:06 로봇 상태 idle                                      ∧      │
└───────────────────────────────────────────────────────────────────┘
```

- 카메라는 중앙에 꽉 채우고 레터박스는 `bg0`. 신호 없음/오래됨 상태는 §5.9.
- **텔레메트리 칩** 좌상단 16 여백, 높이 28, `scrim` 바탕 + 숫자 폰트. 값이 임계(RTT > 100 ms, 유실 > 5 %, 프레임 나이 > 1 s, 스캔 > 1 s)를 넘으면 칩 테두리만 `warn`/`danger`.
- **주행 상태 캡슐**(§5.4) 하단 중앙-좌, 폭 420 높이 56. 데드맨·상태·사유를 한 줄로. 명령 막대는 캡슐 위 2개, 가운데 눈금 + 채움 방향.
- 경로 진행 중이면 카메라 상단 중앙에 `info` 배너 "경로 이동 중 3/7 · 남은 2.1 m · 스틱을 쓰면 취소".
- 레이더 PIP: 320×320, 모서리 16, 외곽 1 px `line`, 바탕 `scrim`. Y로 "카메라 / 카메라+레이더 / 레이더만" 순환은 유지. 레이더만 모드는 중앙 600×600.
- 오른쪽 레일 버튼은 아이콘 + 라벨 + 패드 글리프(Ⓨ, RB, Ⓐ). Ⓐ 서기는 RT 쥔 동안 비활성(회색 + 사유 툴팁 대신 캡슐에 "운전 중에는 서기 불가").

### 4.2 지도·경로(Map)

```
┌────────┬────────────────────────────────────────────────┬─────────┐
│        │ ╭⚠ 로봇은 장애물을 피하지 않습니다╮               │ ➤ 경로   │
│        │                                                │  보내기 │
│        │     지도 JPEG (bg0 레터박스, 1 px line 테두리)   │ ✕ 취소 Ⓧ│
│        │       · 손가락 스케치: accent 4 px             │ ⌫ 지우기 │
│        │       · 단순화 경유점: ok 원 9 px, 번호          │ ⊕ ×1 ×2 ×4│
│        │       · 로봇 실행 경로: info 3 px, 현재점 warn   │ ─────── │
│        │       · 로봇: 흰 테두리 + accent 삼각(방향)      │ ⤓ 지도저장│
│        │                                                │ ↺ 지도새로│
│        │  ┌──────────────────────────────────────────┐   │ ┌─────┐ │
│        │  │ 경로 없음 · 새 경로 13점 11.5 m · 보내기 가능 │   │ │비상 │ │
│        │  └──────────────────────────────────────────┘   │ │정지 │ │
├────────┴────────────────────────────────────────────────┴─────────┤
```

- 지도 배경을 중앙 전체 `bg0`로 통일(D11). 지도 이미지 자체는 흰/검/회 원본 유지하되 외곽 1 px `line`.
- 경고는 상단 좌측 `warn` 테두리 칩(아이콘 ⚠ + 글자, 바탕 `scrim`)으로 고정. 노란 맨글자 제거.
- 하단 상태 캡슐: "경로 상태 · 새 경로 n점 길이 · **보내기 가능/불가(사유)**". 보내기 불가 사유 = "중립 확인 중", "경로 실행 중", "스케치 없음".
- 확대는 **세그먼트 컨트롤** ×1 / ×2 / ×4(§5.3) — 버튼 하나를 눌러 순환하는 현재 방식보다 상태가 보인다.
- "지도 새로"는 2단계 확인 유지. 확인 상태에서는 버튼이 `warn` 면 + "정말 새로?" + 3 s 뒤 자동 복귀.

### 4.3 로봇(Robot)

```
┌────────┬────────────────────────────────────────────────┬─────────┐
│        │ 자세 프리셋                        D-pad ▲▼     │ ⌂ 서기 Ⓐ│
│        │ ┌─────┐┌─────┐┌─────┐┌─────┐┌─────┐┌─────┐┌─────┐│ ⛔ 전체  │
│        │ │ ⬆ 서기││ ⬇ 엎드││ ◢ 10°││ ◢ 20°││ ◢ 30°││ ⬇ 낮게││ ⌂ home ││   정지  │
│        │ └─────┘└─────┘└─────┘└─────┘└─────┘└─────┘└─────┘│ ✓ 전체  │
│        │ 동작 그룹   큰 동작은 ⚠ 표시, 두 번 눌러 실행  LB │   해제  │
│        │ ┌────┐┌────┐┌────┐┌────┐┌────┐┌────┐              │         │
│        │ │ 손 ││ 앉기││ ⚠점프││ …  ││    ││    │  4행 × 6열    │         │
│        │ └────┘└────┘└────┘└────┘└────┘└────┘              │         │
│        │ ┌────┐┌────┐┌────┐┌────┐┌────┐┌────┐              │         │
│        │ …                                                │ ┌─────┐ │
│        │ ▸ 직접 자세 만들기 (접힘)                        │ │비상 │ │
│        │ 상태: idle · 동작 없음 · 중재기 ready            │ │정지 │ │
├────────┴────────────────────────────────────────────────┴─────────┤
```

- 프리셋 7개는 한 줄 7열(각 140×64), 아이콘 + 라벨. 선택 중인 프리셋은 `accent` 외곽 3 px(채움 아님).
- 동작 그룹 24개는 **4행 × 6열 = 스크롤 없이 한 화면**(각 166×56, 간격 8). 스크롤바 공간을 폭 계산에서 뺀다(D9).
- **주의 스타일**: `caution` 동작은 좌상단 ⚠ 배지 + 외곽 1 px `warn`. 1차 탭 후 "무장" 상태는 면 `warn`(ink 글자) + 라벨 "다시 눌러 실행" + 2 s 카운트다운 링(§5.4와 같은 링). 지금의 갈색 채움 제거.
- D-pad ◀▶로 고른 동작은 격자에서 `accent` 외곽으로 표시되어 왼쪽 레일 칩과 일치.
- 전체 정지/해제는 오른쪽 레일로 이동(기존 함수 그대로 호출). 슬라이더 패널은 접힘 기본.
- "/puppy_control/status" 원문 키-값은 하단 1줄 요약 + 로그 화면으로 이동.

### 4.4 로그(Log)

```
┌────────┬────────────────────────────────────────────────┬─────────┐
│        │ ┌ RTT ──────┐┌ 유실 ─────┐┌ 카메라 ───┐┌ 화면 ──┐ │ ◉ 패킷   │
│        │ │ 18.1 ms   ││ 0.0 %     ││ 10.0 fps  ││120 fps│ │  기록   │
│        │ │ p95 20.9  ││ 6995/6991 ││ 나이 98 ms││ 8.3 ms│ │ ≣ 토픽   │
│        │ └───────────┘└───────────┘└───────────┘└───────┘ │ ↺ RTT   │
│        │ ┌ ACK RTT (ms) ────────────────── 중앙 18 · 최대 42 ┐ │  초기화 │
│        │ │  ▁▂▁▁▂▁▃▁▁▂▁▁▁▂▁▁▁▂▁▁▂▁▁▁▂▁▁▁  (채움 영역 그래프) │ │         │
│        │ └──────────────────────────────────────────────┘ │         │
│        │ 연결  v2 192.168.x.x:5001 · client d3878adf · seq 6995│         │
│        │       rosbridge connected · 지도 있음 · 스캔 450빔     │         │
│        │       중재기 owner – · estop ✕ · engine ready · mock ✓ │ ┌─────┐ │
│        │ ┌ 이벤트 ───────────────────────────────────────┐ │ │비상 │ │
│        │ │ 09:37:06.741 ● 조종 연결 시작 …                │ │ │정지 │ │
├────────┴────────────────────────────────────────────────┴─────────┤
```

- **통계 타일** 4개(§5.7 게이지의 텍스트형): 큰 값 `num_lg` + 보조 `num_sm`.
- RTT 플롯은 `PlotLines` 대신 드로우 리스트 영역 그래프(§5.7). 임계 100 ms 가로 점선.
- 중재기 JSON은 키 5개만 아이콘 + 값으로 풀어서 표시, 원문은 패킷 기록 켜면 아래 패널에.
- 이벤트 목록은 레벨 아이콘(● info / ⚠ warn / ✕ error)을 앞에 붙인다. 레벨은 문자열 접두("거절", "끊김", "실패" 포함 시 warn/error)로 임시 판정 → 추후 `LogRing`에 레벨 필드 추가(코어 변경이므로 별도 결정).

### 4.5 연결·설정(Connect)

```
┌────────┬────────────────────────────────────────────────┬─────────┐
│        │ ┌ 로봇 찾기 ─────────────────┐ ┌ 주소 직접 입력 ───────┐│ ⏻ 앱    │
│        │ │ [⟳ 같은 Wi-Fi에서 찾기]    │ │ 192.168.x._        ││  끝내기 │
│        │ │ ┌──────────────────────┐   │ │ ┌───┐┌───┐┌───┐      ││ (View+  │
│        │ │ │ ● puppypi  192.168…  │ → │ │ │ 1 ││ 2 ││ 3 │      ││  Menu)  │
│        │ │ └──────────────────────┘   │ │ ├───┤├───┤├───┤      ││         │
│        │ │ 이 기기: 192.168.x.10    │ │ │ 4 ││ 5 ││ 6 │ 64×64││         │
│        │ └────────────────────────────┘ │ ├───┤├───┤├───┤      ││         │
│        │ ┌ 설정 ──────────────────────┐ │ │ 7 ││ 8 ││ 9 │      ││         │
│        │ │ [○ 한 스틱 운전] [○ 기울임]│ │ │ . ││ 0 ││ ⌫ │      ││         │
│        │ │ 최고 속도  ━━━━━●━━  60 %  │ │ [지우기] [● 연결]     ││ (비상정지│
│        │ │ 데드존     ━━●━━━━━  0.12  │ └──────────────────────┘│  없음:  │
│        │ │ [● 카메라 영상 받기]        │  연결됨이면 [연결 끊기] │  미연결) │
├────────┴────────────────────────────────────────────────┴─────────┤
```

- 두 카드(찾기 / 직접 입력) 좌우 배치, 설정 카드는 아래. 체크박스는 **토글 스위치**(§5.3의 세그먼트와 같은 그리기)로.
- 연결 화면에서는 운전하지 않으므로 상태띠 주행 캡슐은 "연결 화면 · 운전 안 함"(info).
- 비상정지 버튼은 연결된 상태면 그대로 표시, 미연결이면 비활성(회색, "미연결").

---

## 5. 컴포넌트와 드로우 리스트 구현

공통: `ImDrawList* dl = ImGui::GetWindowDrawList()`, 모든 수치에 `s_`(DPI) 곱. 히트 영역은 `ImGui::InvisibleButton(id, size)`로 만들고 `IsItemHovered/Active/Activated`로 상태를 읽은 뒤 그 사각형 위에 그린다. 이렇게 하면 터치·마우스·키보드 동작은 ImGui가 그대로 처리한다.

### 5.1 상태 칩 `chip(icon, text, tone, filled)`
- 높이 28, 좌우 패딩 10, 모서리 14(캡슐).
- `filled`: `AddRectFilled(p0,p1,tone,14)` + ink 글자. 비채움: `AddRect(p0,p1,tone,14,0,1.5)` + tone 글자 + 바탕 `bg2`.
- 아이콘은 Segoe Fluent 글리프 또는 점(`AddCircleFilled(c,5,tone)`). 연결 칩은 점.
- 주행 상태 캡슐은 이 칩의 **큰 변형**(높이 36, 글자 `button` 크기)이고 상태띠에서 유일하게 채움.

### 5.2 아이콘 버튼 `icon_button(id, icon, label, pad_glyph, size, tone, enabled)`
- 그림자 2장 → 면(`bg2`→`bg3` 세로 그라데이션, hover 시 +6 %, active 시 −8 %) → 윗변 하이라이트 1 px → 아이콘(좌 16, 28 px) → 라벨(아이콘 우 12, `button`) → 우측 끝 패드 글리프.
- 비활성: 면 `bg1`, 글자 text3, 그림자 없음. 비활성 사유는 캡슐/토스트로.
- **패드 글리프 `pad_glyph(kind, pos, size=22)`**: ABXY = `AddCircleFilled(c, r, bg3)` + `AddCircle(c, r, text2, 0, 1.5)` + 중앙 글자(Bold, 14) 흰색. Xbox 관례색을 작은 점으로만(아래 4 px 점: A 초록, B 빨강, X 파랑, Y 노랑) 넣어 브랜드 아트워크를 모사하지 않으면서 식별성 확보. LB/RB/LT/RT = 라운드 사각(22×14, r 4) + 글자 10. D-pad = 십자 `AddRectFilled` 2개 + 활성 방향 삼각. 이 함수 하나로 모든 힌트를 그려 일관성 확보.

### 5.3 세그먼트 컨트롤 `segmented(id, items[], &index, w, h=44)`
- 트랙: `AddRectFilled(bg1, r=h/2)` + 1 px `line`.
- 선택 세그먼트: `AddRectFilled(accent, r=h/2−3)`, 글자 ink. 선택 위치 x는 160 ms 보간.
- 각 세그먼트는 `InvisibleButton`. 최소 폭 64. 토글 스위치는 2세그먼트 변형(폭 64, 노브 원).

### 5.4 비상정지 버튼 `estop_button()` (동작은 기존 `estop_all()`/`clear_emergency_all()` 그대로)
- 크기 120×120, 모서리 20.
- **정상(누르면 정지)**: 바깥 링 4 px `warn`(ISO 13850의 "빨강 위 노랑" 기호성) → 그림자 2장 → 면 `danger`→`danger_deep` 세로 그라데이션(ShadeVerts) → 윗변 하이라이트 → 중앙 팔각 정지 아이콘 36 px(`AddNgonFilled(c, 18, white, 8)` 위에 `AddNgon` 1.5 px) → 아래 "비상정지" Bold 26 흰색(큰 글자이므로 3.9 : 1 허용, 그라데이션 하단부는 6.5 : 1) → 우하단 패드 글리프 Ⓑ.
- 눌림: 면을 `danger_deep` 단색, 0.6 s 럼블은 기존.
- **비상정지 중(해제 대기)**: 면 `bg2`, 링 `danger` 4 px 깜빡(0.8 s 2회 후 고정), 글자 "해제: 1초 누르기" text1, 아이콘 자물쇠. 누르는 동안 **진행 링**: `dl->PathArcTo(c, 52, -π/2, -π/2 + 2π·k, 48); dl->PathStroke(ok, 0, 6)` + 중앙 숫자 "0.6 s". k ≥ 1에서 기존 로직대로 해제 호출, `clear_hold_start_ = −1` 유지.
- 해제 후 `release_required` 동안 캡슐에 "RT를 한 번 놓았다 다시 쥐세요"(warn) — 기존 문구 유지.
- 미연결: 면 `bg1`, 글자 text3 "미연결".

### 5.5 주행 상태 캡슐 `drive_state_capsule(snapshot, held, source)` — 안전 정보의 핵심
한 줄 = `[램프] [상태어] · [입력원] · [사유]`. 램프는 지름 14 원 + 바깥 글로우 링 1장(α 60).

| 상황 | 램프/면 | 글자 |
|---|---|---|
| RT 안 쥠, idle | 테두리 info, 면 scrim | "○ 대기 · RT를 쥐면 운전" |
| RT 쥠, cmd_active | 면 **ok**, ink 글자 | "● 주행 중 · 패드 · 60 %" |
| RT 쥠, 로봇이 거절/중립 확인 중 | 면 **warn** | "◐ 운전 대기 · 중립 확인 중" / "다른 조종기가 제어 중" |
| release_required | 면 warn | "◐ RT를 놓았다 다시 쥐세요" |
| 포커스 없음/연결 화면 | 테두리 text3 | "○ 운전 안 함 · 창 포커스 없음" |
| ACK 끊김/비상정지/fault | 면 **danger**, 흰 Bold | "✕ 비상정지" / "✕ 응답 없음" |

명령 막대 2개(전후·회전)는 캡슐 위: 트랙 6 px `bg3` 캡슐, 중앙 눈금 2 px text2, 채움은 상태색(활성 ok, 비활성 text3), 끝에 값 `num_sm`. 막대 폭 300.

### 5.6 토스트 `toast(text, tone)`
- 중앙 상단(카메라 위 y=16), 최대 폭 640, 높이 44, 모서리 22, 면 `bg2` α 0.95 + 왼쪽 4 px tone 바 + 아이콘 + 글자 text1. 등장 120 ms(y −8→0, α 0→1), 3 s, 페이드 200 ms. 동시에 하나만(기존 `toast_`/`toast_until_` 재사용). 비상정지 관련 토스트는 tone danger로 5 s.

### 5.7 텔레메트리 게이지·플롯
- **타일**: 카드(§3.4) 안에 라벨 `label` + 큰 값 `num_lg` + 보조 `num_sm` text2. 임계 초과 시 값 글자색만 warn/danger.
- **미니 바 게이지**(RTT 칩 안): 폭 48 높이 6, 0-100 ms 선형, 초과분은 danger.
- **RTT 영역 그래프**: `PathLineTo`로 상단 선(`info`, 2 px) + 같은 점열 + 바닥 두 점으로 `AddConvexPolyFilled`가 아닌 **세로 띠 `AddRectFilled` n개**(값 600개 → 폭에 맞춰 2-3 px 간격 버킷 평균) α 60. 임계선 `AddLine` 점선(길이 6 간격 4). 우상단 요약 "중앙 18 · p95 21 · 최대 42".

### 5.8 레이더(LiDAR PIP) 스타일
- 바탕 원 `bg0` α 0.7, 격자 원 6개 `lidar` α 0.25 1 px, 십자 α 0.15, 0.5 m마다 작은 라벨은 1 m·2 m·3 m만(우측 수직선 옆, `num_sm` text2, 그림자 포함).
- 스캔 점: 반지름 2 px, 색 `lidar`; 0.35 m 이내는 `danger`; 1 s 이상 오래된 스캔은 text3 + 중앙 상단 "SCAN 1.4 s" 칩 warn.
- 로봇 몸: `AddRect` 흰 α 0.9 1.5 px + 전방 삼각 `accent`. 라벨 "3 m"는 원 바깥 좌상단 칩(겹침 해결, D8).
- 전체 PIP 외곽 `AddRect(line, r=16)`.

### 5.9 경로 스타일(지도)
- 스케치: `accent` 4 px, `AddPolyline(..., ImDrawFlags_RoundCornersAll)`.
- 단순화 경유점: 원 r 9 `ok` 외곽 3 px + 내부 번호(`num_sm`, 그림자). 선은 `ok` 2 px.
- 로봇 실행 경로: 남은 구간 `info` 3 px, 지난 점 text3 채움, 현재 목표점 `warn` r 11 + 바깥 링 펄스 없음(정지 표시).
- 로봇 마커: 흰 외곽 원 r 10 + `accent` 방향 삼각.

### 5.10 빈 상태 / 무신호 / 연결 상태
- 카메라 없음: 중앙 아이콘(카메라 슬래시 64 px text3) + "카메라 신호 없음" `title` text2 + 아래 hint "설정에서 카메라 영상 받기 확인". 바탕 `bg0`에 미세한 대각 해칭 없음(단색).
- 카메라 오래됨(> 2 s): 마지막 프레임을 60 % 밝기로 유지(기존) + 좌상단 칩 warn "영상 3초 전" + 프레임 외곽 2 px warn.
- 지도 없음: 지도 아이콘 + "지도 기다리는 중" + hint "로봇이 조금 움직이면 생깁니다".
- 연결 상태 3종: 끊김(상태띠 좌측 칩 danger "연결 안 됨" + 캡슐 danger), 연결 중/중립 확인 중(칩 warn + 캡슐 warn "중립 확인 중"), 연결됨(칩 ok 점 + 호스트). 패드 없음은 상태띠 우측 패드 아이콘에 슬래시 + danger, 그리고 주행 화면 캡슐 "○ 패드 없음 · Armoury Crate 게임패드 모드 확인".

---

## 6. ImGui 1.93 구현 계획

### 6.1 `src/win/theme.hpp / theme.cpp`

```cpp
namespace hg::theme {
struct Tokens { ImU32 bg0, bg1, bg2, bg3, line, text1, text2, text3, accent, ok, warn, danger, danger_deep, info, lidar, ink; };
extern const Tokens T;                         // §3.1 HEX
struct Fonts { ImFont* body; ImFont* bold; ImFont* num; };  // icon은 body/bold에 병합
Fonts load_fonts(const std::string& windir_fonts, const std::string& exe_dir);
void apply_style(float dpi_scale);             // ImGuiStyle 전부 + Colors[] 덮어쓰기
// 크기 프리셋(논리 px). 사용: ImGui::PushFont(nullptr, theme::sz::title)
namespace sz { constexpr float body=18, label=15, hint=14, title=22, button=17, num_lg=28, num_sm=16, estop=26, icon=20, icon_lg=28; }
ImU32 alpha(ImU32 c, float a);                 // 알파만 바꾼 색
}  // namespace
```

폰트 로드(모두 **크기 인자 없이** 로드, 1.92+ 동적 폰트; 글리프 범위 지정 불필요 — [FONTS.md](https://github.com/ocornut/imgui/blob/master/docs/FONTS.md)):

```cpp
ImGuiStyle& st = ImGui::GetStyle();
st.FontSizeBase = sz::body; st.FontScaleDpi = dpi_scale;
auto add = [&](const char* path, ImFontConfig* cfg = nullptr) { return file_exists(path) ? io.Fonts->AddFontFromFileTTF(path, 0.0f, cfg) : nullptr; };
// 1) 본문: 번들 Pretendard가 있으면 그것, 없으면 malgun
f.body = add(exe_dir + "\\fonts\\Pretendard-Regular.ttf") ?: add(fonts + "malgun.ttf") ?: io.Fonts->AddFontDefault();
// 2) 아이콘 병합(본문 뒤에): Segoe Fluent Icons → 없으면 Segoe MDL2 → 없으면 번들 아이콘
ImFontConfig ic; ic.MergeMode = true; ic.GlyphOffset = ImVec2(0, 2 * dpi_scale);  // 베이스라인 보정은 실측 후 조정
ic.GlyphMinAdvanceX = 0;  // 고정폭 아이콘이 필요하면 크기를 명시해야 함(FONTS.md)
add(fonts + "SegoeIcons.ttf", &ic) ?: add(fonts + "segmdl2.ttf", &ic);
ImFontConfig sym; sym.MergeMode = true; add(fonts + "seguisym.ttf", &sym);   // ▲▼◀▶ 등 기존 기호 유지
// 3) 굵게: 같은 순서로 Pretendard-Bold / malgunbd + 아이콘 병합
// 4) 숫자: CascadiaMono.ttf → consola.ttf (숫자 폭이 같아 RTT가 흔들리지 않음)
```

- Segoe Fluent Icons는 Windows 11에 기본 포함, 파일명은 `SegoeIcons.ttf`(실기에서 `%WINDIR%\Fonts` 확인 필요). MS 문서상 "다른 플랫폼으로 배포 금지"이므로 **실행 시 시스템에서 읽기만** 한다(현재 malgun과 같은 방식). 아이콘 코드포인트는 `theme.hpp`에 `ICON_CAMERA u8""` 식 상수로(Camera E722, MapPin E707, MapLayers E81E, Robot E99A, Settings E713, List EA37, Wifi E701, Connect E703, Warning E7BA, Error E783, Play E768, Stop E71A, Game E7FC, Bullseye F272, StatusCircleCheckmark F13E, StatusCircleErrorX F13D, StatusTriangleExclamation F13B, StatusCircleBlock F140, StatusCircleSync F143).
- 아이콘 글리프가 없을 때(Windows 10, 다른 PC) 대비: `ImFont::IsGlyphInFont()`로 시작 시 점검해 `theme::icons_ok=false`면 아이콘 자리에 2글자 약어(주/지/로/로/연)를 그린다.
- Pretendard 번들 시 `fonts/` 폴더 + `fonts/LICENSE-Pretendard.txt`(OFL 전문, Reserved Font Name 유지) + `THIRD_PARTY.md` 행 추가. Material Symbols/Lucide를 번들하면 각각 Apache-2.0/ISC 고지.

`apply_style()` 핵심 값: `WindowRounding 0, ChildRounding 12, FrameRounding 10, GrabRounding 12, PopupRounding 12, ScrollbarRounding 8, FramePadding (12,10), ItemSpacing (12,10), ScrollbarSize 20, GrabMinSize 32, WindowBorderSize 0, FrameBorderSize 0`, 이후 `ScaleAllSizes(dpi)`. `Colors[]`: WindowBg/ChildBg=bg0, FrameBg=bg2, FrameBgHovered=bg3, Button=bg2, ButtonHovered=bg3, ButtonActive=bg1, SliderGrab=accent, SliderGrabActive=accent, CheckMark=accent, Header=bg2, Separator=line, ScrollbarBg=bg1, ScrollbarGrab=bg3, Text=text1, TextDisabled=text3, PlotLines=info, NavHighlight=accent(포커스 링). 기본 위젯(슬라이더·체크박스)은 이 값만으로도 통일된다.

### 6.2 `src/win/widgets.hpp / widgets.cpp`

```cpp
namespace hg::w {
enum class Tone { Neutral, Accent, Ok, Warn, Danger, Info };
enum class Pad { A, B, X, Y, LB, RB, LT, RT, LS, RS, DpadUD, DpadLR, View, Menu };
void shadow(ImDrawList*, ImVec2 p0, ImVec2 p1, float r);
void card(ImDrawList*, ImVec2 p0, ImVec2 p1, float r = 12);                // 그림자+면+외곽
void text_shadowed(ImDrawList*, ImVec2 pos, ImU32 col, const char* txt);
void pad_glyph(ImDrawList*, Pad, ImVec2 center, float size, bool emphasized = false);
bool chip(const char* id, const char* icon, const char* text, Tone, bool filled, bool clickable = false);
bool icon_button(const char* id, const char* icon, const char* label, ImVec2 size, Tone = Tone::Neutral, const Pad* hint = nullptr, bool enabled = true);
bool segmented(const char* id, const char* const* items, int n, int* index, ImVec2 size);
bool toggle(const char* id, const char* label, bool* v);
bool estop(const char* id, float size, bool engaged, bool connected, float hold_k /*0..1, <0이면 미홀드*/, bool* hold_active);
void drive_capsule(ImVec2 pos, float w, Tone, const char* lamp, const char* text);
void cmd_bar(ImVec2 pos, float w, const char* name, double v, bool active);
void stat_tile(const char* label, const char* big, const char* small, ImVec2 size, Tone = Tone::Neutral);
void area_plot(const char* id, const float* v, int n, float vmin, float vmax, float threshold, ImVec2 size);
void toast(ImVec2 anchor, const char* text, Tone, float t_alive, float t_total);
void empty_state(ImVec2 center, const char* icon, const char* title, const char* hint);
}
```

모든 위젯은 "InvisibleButton → 상태 읽기 → 드로우 리스트"의 동일 패턴. `ImGui::GetStyle().Alpha`와 `BeginDisabled`를 존중(색에 `Alpha` 곱).

### 6.3 `ui.cpp` 변경(기능·입력 매핑은 손대지 않음)

| 함수 | 변경 |
|---|---|
| `draw()` | 레이아웃 상수 `top 44, bottom 28, rail_l 80, rail_r 128`(×`s_`). 이벤트 패널 펼침 상태 `events_open_` 추가(헤더 멤버 1개) |
| `draw_status_bar()` | 칩 람다 제거 → `w::chip`, 캡슐 → `w::drive_capsule`, 토스트 호출 제거(§5.6으로 이동) |
| `draw_left_rail()` | `big_button` → `w::icon_button`(세로형, 아이콘+라벨), 하단 3칩 |
| `draw_right_rail()` | `w::icon_button` + 패드 힌트, 확대 → `w::segmented`, `estop_button` 교체 |
| `estop_button()` | 상태 계산(기존 `clear_hold_start_` 로직)은 유지, 그리기만 `w::estop`로 |
| `draw_drive()` | 칩 3개, 캡슐, `cmd_bar`, 경로 배너, 레이더 PIP 테두리 |
| `draw_radar()` | 색 토큰·라벨 위치·PIP 외곽 |
| `draw_map()` | 배경 `bg0`, 경고 칩, 하단 캡슐, 경로 색 토큰 |
| `draw_robot()` | 7열/6열 격자 폭 = `(avail − ScrollbarSize − gaps)/cols`, `w::icon_button` + ⚠ 배지, 무장 링 |
| `draw_log()` | `stat_tile` ×4, `area_plot`, 중재기 5키 요약 |
| `draw_connect()` | 카드 2+1, `w::toggle`, 키패드 `icon_button` |
| `big_button()` | 삭제(모두 교체 후) |

`app_main.cpp`: 스타일·폰트 블록(233-262행)을 `theme::apply_style(scale)` + `theme::load_fonts(...)` 두 호출로. `Makefile`의 `WIN := app_main gamepad tilt textures ui theme widgets`.

### 6.4 단계(각 단계 끝이 빌드·실행 가능) 와 공수

| 단계 | 내용 | 공수 | 완료 기준 |
|---|---|---|---|
| S1 | `theme.*` 토큰·`apply_style`·폰트 로드(시스템 폰트 경로), 레이아웃 상수 변경 | 4 h | 기존 화면이 새 색·폰트·여백으로 뜸. 아이콘 글리프 1개 표시 확인 |
| S2 | `widgets.*` 중 `shadow/card/text_shadowed/pad_glyph/chip/icon_button` + 좌우 레일·상태띠 교체 | 5 h | 레일에 아이콘·패드 힌트, 상태띠 칩 정리 |
| S3 | `estop` + `drive_capsule` + `cmd_bar` (안전 표시 우선) | 4 h | 비상정지 6상태 스크린샷, 해제 링 동작, `decide_drive()` 무변경 확인 |
| S4 | 주행 HUD(칩·배너·레이더 PIP·빈 상태) + 토스트 + 하단 이벤트 1줄/펼침 | 4 h | drive.png 재촬영 |
| S5 | 로봇 화면 격자·주의 스타일·무장 링 | 3 h | 24 버튼 스크롤 없이, 잘림 없음 |
| S6 | 지도 화면 | 3 h | 세그먼트 확대, 경고 칩, 상태 캡슐 |
| S7 | 로그 화면 타일·영역 그래프 | 3 h | |
| S8 | 연결·설정 카드·토글·키패드 | 3 h | |
| S9 | 모션(보간)·120 Hz 프레임 시간 측정·Pretendard 번들 여부 반영·THIRD_PARTY 갱신 | 3 h | 로그 화면 "화면 fps" 120 유지, 프레임 ≤ 4 ms |
| S10 | 포트폴리오 캡처(§8) | 2 h | |
| 합계 | | **약 34 h** | |

S1-S3까지만 해도 "기본 ImGui" 인상은 사라지고 안전 표시가 개선되므로, 여기서 한 번 실기 점검을 받는 것을 권한다.

---

## 7. 안전·피드백 불변 조건

- **비상정지는 모든 화면에서 가장 크고(120×120) 가장 채도 높은 유일한 요소**. 다른 요소는 danger 채움을 쓰지 않는다(테두리·글자만). 위치 우하단 고정, 오른쪽 엄지 도달 범위.
- 주행 상태 캡슐은 주행 화면뿐 아니라 **상태띠에도 같은 색·같은 문구**로 항상 보인다. 색 + 램프 모양(○ ◐ ● ✕) + 3단어 글자 세 겹.
- 비상정지 중에는 상태띠 바탕이 `danger_deep`(기존 동작 유지), 캡슐 danger, 비상정지 버튼은 해제 모드.
- 로직 불변: `handle_input()`, `feedback()`, `decide_drive()`, 럼블 패턴, 패드 매핑, 해제 1 s 홀드, 큰 동작 2 s 재프레스, 지도 새로 2단계 확인, 포커스 상실 중립은 **한 줄도 바꾸지 않는다**. 재설계는 `draw_*`와 스타일 코드에만 닿는다. S3 완료 시 `make test` 63개 통과와 `it_drive.txt` 14/14를 다시 확인한다(UI 변경이 코어를 건드리지 않았다는 증거).
- 글자 크기는 어디서도 14 논리(21 px) 미만으로 내리지 않는다. 안전 관련 글자(캡슐·비상정지·거절 사유)는 17 이상 Bold.
- 모션은 안전 정보에 지연을 더하지 않는다(§3.5).
- 화면 비상정지는 B 버튼·로봇 watchdog의 **보조 수단**이며 1차 비상정지 장치가 아니라는 문장을 README 안전 모델 절에 유지한다(산업 규격상 HMI 소프트 버튼은 1차 비상정지로 인정되지 않음).

---

## 8. 포트폴리오 제시

- **스크린샷 5장**(F12 → `shots/ui_*.bmp`, 1920×1080 원본): 주행(레이더 PIP + 주행 중 캡슐 ok), 주행-비상정지 상태(상태띠 진홍 + 해제 링 60 %), 지도(스케치 + 경유점 번호 + 실행 경로), 로봇(격자 + 무장된 큰 동작 1개), 로그(타일 + 영역 그래프). 실제 로봇 영상이 있는 프레임을 우선하고, mock이면 README 캡션에 "mock 로봇" 명시.
- **GIF 2개(각 ≤ 8 s, 1280×720로 축소)**: (1) RT 쥐기 → 캡슐 회색→초록, 막대 채움, 놓기 → 즉시 회색. (2) B 비상정지 → 상태띠 진홍 → 해제 1 s 홀드 링 → "RT 다시 쥐세요" → 재개. 녹화는 Xbox Game Bar(Win+Alt+R) 또는 백버퍼 BMP 연속 저장 + ffmpeg(별도 결정).
- **README 히어로 이미지**: 검은 배경 위에 흑갈리 실물 사진(사용자 촬영, 화면 켜진 상태)과 그 옆에 주행 화면 스크린샷을 기기 화면 비율로 합성, 아래 한 줄 "RT를 쥐어야 움직인다 — 데드맨·햅틱 ACK·LiDAR HUD". 기기 사진이 없으면 주행 스크린샷 단독 + 캡슐·비상정지에 얇은 주석선 3개.
- 재설계 전/후 비교 1장(현재 `docs/screenshots/drive.png` 보존)을 `docs/` 에 두어 설계 판단 과정을 보여 준다.

---

## 9. 리스크와 대응

| 리스크 | 영향 | 대응 |
|---|---|---|
| **폰트 라이선스** — Segoe Fluent Icons·맑은 고딕은 재배포 불가(실행 시 시스템 로드만 가능). Pretendard OFL은 번들 가능하나 Reserved Font Name 조건, Cascadia Code도 OFL | 저장소 공개 시 위반 가능 | 시스템 폰트는 지금처럼 경로에서만 읽음. 번들은 OFL/Apache/ISC만, 라이선스 전문 동봉, `THIRD_PARTY.md` 갱신. 폰트 변형(subset) 시 OFL Reserved Name 규정 확인 |
| **아이콘 글리프 부재** — 맑은 고딕에는 UI 아이콘·패드 글리프 없음. Segoe Fluent Icons는 Windows 11 전용, 파일명·설치 여부는 실기 확인 필요 | 네모(tofu) 표시 | 시작 시 `IsGlyphInFont` 점검 → 약어 폴백. 패드 글리프는 드로우 리스트로 직접 그려 폰트 무관. 필요 시 Material Symbols/Lucide 번들로 전환(결정 항목) |
| **아이콘 베이스라인** — 병합 폰트의 세로 정렬이 어긋남 | 아이콘이 위/아래로 뜸 | `GlyphOffset.y` 실측 조정(FONTS.md에는 지침 없음), 아이콘은 가급적 `AddText`로 직접 위치 지정 |
| **120 Hz 성능** — 그림자 2장·그라데이션·영역 그래프 600막대가 드로우 콜·정점 증가 | 프레임 8.3 ms 초과, 배터리 | 그림자는 2장 고정, 플롯은 화면 폭 버킷(≤ 400 막대), 텍스트 그림자는 HUD 글자에만. 로그 화면 "화면 fps"·`io.DeltaTime`로 S9에서 측정, 기준 ≤ 4 ms. 1.93의 동적 폰트 아틀라스는 새 크기를 처음 쓸 때 재생성하므로 **크기 프리셋 9개만 사용**(임의 크기 금지) |
| **터치 타깃** — 레일 폭 축소(168→80)로 라벨이 짧아지고, 7열 격자 버튼 140 폭은 손가락에 충분하나 라벨 길이 제한 | 오탭, 라벨 잘림 | 모든 타깃 ≥ 48 논리(72 px), 간격 ≥ 8. 라벨은 4글자 이내로 정리("지도·경로"→"지도", "연결·설정"→"연결"). 잘리면 `CalcTextSize`로 자동 축소 금지 대신 말줄임 |
| **대비** — accent·info 위 흰 글자는 WCAG 미달(2.8/2.75 : 1) | 가독성 | 채움 칩 위 글자는 반드시 ink. 규칙을 `chip()` 안에서 강제 |
| **TV/실기 색 차이** — Ally 패널에서 `bg0`/`bg1` 차이(1.26 : 1)가 뭉개질 수 있음 | 레이어 구분 상실 | 1 px `line` 외곽을 항상 함께 그림. 실기에서 밝기 50 %/100 % 두 조건 확인 |
| **ImGui 내부 API 의존** — `ShadeVertsLinearColorGradientKeepAlpha`는 `imgui_internal.h` | 버전 업 시 시그니처 변경 가능 | `widgets.cpp` 한 곳에서만 호출, 실패 시 단색으로 폴백하는 매크로 |
| **멀티터치 없음**(기존 제약) | 세그먼트·슬라이더 동시 조작 불가 | 변경 없음, 설계상 동시 조작 요구 없음 |
| 스크린샷이 mock 로봇 기준 | 포트폴리오 신뢰 | 캡션에 조건 명시(README 규칙과 동일) |

---

## 10. 사용자 결정 항목

1. **본문 폰트**: 시스템 맑은 고딕 유지(번들 0) vs Pretendard Regular+Bold 번들(약 2.5-3 MB, OFL). 권장: Pretendard.
2. **아이콘 출처**: Segoe Fluent Icons 시스템 로드(Win11 전용, 번들 0) vs Material Symbols Rounded 또는 Lucide 번들(수백 KB). 권장: Segoe Fluent 우선 + 폴백, Windows 10 지원이 필요해지면 번들.
3. **강조색**: 주황 `#FF6B35`(검은 섀시 + 로봇 "주의" 톤) vs 청록 `#19C6D1`(LiDAR와 통일). 권장: 주황(LiDAR 청록과 역할 분리).
4. **왼쪽 레일**: 아이콘+짧은 라벨 80 폭(권장) vs 아이콘 전용 56 폭.
5. **하단 이벤트 띠**: 1줄 + 펼치기(권장) vs 완전 제거하고 로그 화면만.
6. **패드 글리프에 Xbox 관례색 점(A 초록·B 빨강·X 파랑·Y 노랑)** 포함 여부. 브랜드 아트워크 모사는 아니지만 보수적으로 가려면 단색.
7. `LogRing`에 레벨 필드를 추가할지(코어 변경, 단위 시험 영향 없음) — 아니면 문자열 휴리스틱 유지.
8. GIF 녹화 방법(Xbox Game Bar vs 백버퍼 연속 저장 + ffmpeg).

---

## 참고 출처 목록

- Boston Dynamics Spot Tablet: https://bostondynamics.com/products/extras/spot-tablet/ , Pro: https://bostondynamics.com/products/extras/spot-tablet-controller-pro/ , 사용자 가이드 PDF: https://www.generationrobots.com/media/spot-boston-dynamics/spot-user-guide-r2.0-va.pdf , 설계 배경: https://iotworldtoday.com/robotics/boston-dynamics-spot-the-design-behind-the-robot-dog
- Unitree Go2 앱: https://www.unitree.com/mobile/app/go2 , 운용 가이드: https://ms-kb.msd.unimelb.edu.au/next-lab/robotics/robotic-dog/operation-setup/operation-basics
- Clearpath OutdoorNav Manual Mode: https://docs.clearpathrobotics.com/docs_outdoornav_user_manual/web_user_interface/ui_manual_mode , Assisted Teleop: https://docs.clearpathrobotics.com/docs_outdoornav_user_manual/features/assisted_teleoperation
- ANYbotics: https://www.anybotics.com/robotics/robot-capabilities/
- Foxglove panels: https://docs.foxglove.dev/docs/visualization/panels/introduction , ROS 2 가이드: https://docs.ros.org/en/jazzy/How-To-Guides/Visualizing-ROS-2-Data-With-Foxglove-Studio.html
- ROS-Mobile: https://arxiv.org/abs/2011.02781
- QGroundControl Fly View: https://docs.qgroundcontrol.com/master/en/qgc-user-guide/fly_view/fly_view.html , Instrument Panel: https://docs.qgroundcontrol.com/master/en/qgc-user-guide/fly_view/instrument_panel.html
- Mission Planner Flight Data: https://ardupilot.org/planner/docs/mission-planner-flight-data.html
- Auterion Mission Control Fly View: https://docs.auterion.com/vehicle-operation/auterion-mission-control/ui-breakdown/fly/fly-view-ui-overview
- DJI Fly 가이드(3자): https://www.pix-pro.com/dji-fly-guide-part1
- Microsoft Designing for Xbox and TV: https://learn.microsoft.com/en-us/windows/apps/design/devices/designing-for-tv , Gamepad interactions: https://learn.microsoft.com/windows/uwp/design/input/gamepad-and-remote-interactions , Segoe Fluent Icons: https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-fluent-icons-font
- Steam Deck: https://partner.steamgames.com/doc/steamdeck/faq , https://bencollette.myportfolio.com/steamdeck , 커뮤니티 버튼 프롬프트: https://steamcommunity.com/groups/SteamClientBeta/discussions/0/3710433479218773768
- Armoury Crate SE 소개: https://rog.asus.com/support/FAQ/1050220
- Nintendo Switch UI: https://medium.com/@dli_li/nintendo-switch-ui-design-1eb1742515db , https://www.nintendolife.com/news/2017/01/heres_your_first_look_at_the_nintendo_switch_user_interface
- 게임 HUD: https://pixune.com/blog/game-hud-design/ , https://www.wayline.io/blog/your-ui-is-killing-immersion-fix-it
- 차량 HMI: https://aaafoundation.org/research/human-machine-interfaces-and-vehicle-automation-a-review-of-the-literature-and-recommendations-for-system-design-feedback-and-alerts/
- 비상정지 규격 요약: https://www.controldesign.com/safety/safety-components/article/21526010/standards-guide-the-use-of-estops , https://www.newark.com/pdfs/techarticles/eao/EAO_Emergency_Stops-What_Designers_Need_to_Know.pdf
- Universal Robots 3PE: https://www.universal-robots.com/manuals/EN/HTML/SW5_19/Content/prod-usr-man/complianceUR30/3petp_UR30/3petp-UR30-description.htm
- 터치 타깃 기준 정리: https://www.designsystemscollective.com/a-designers-field-guide-to-wcag-ios-and-android-rules-31ba5d01d46a , axe 규칙: https://dequeuniversity.com/rules/axe/4.6/target-size
- Dear ImGui: FONTS.md https://github.com/ocornut/imgui/blob/master/docs/FONTS.md , 갤러리 #7503 / #7959 / #8333, 창 그림자 #1329 https://github.com/ocornut/imgui/issues/1329 , IconFontCppHeaders https://github.com/juliettef/IconFontCppHeaders
- 폰트 라이선스: Pretendard https://cdn.jsdelivr.net/npm/pretendard@1.3.9/docs/en/README.md (OFL 1.1), Cascadia Code https://learn.microsoft.com/ro-ro/windows/terminal/cascadia-code (OFL), Material Symbols https://developers.google.com/fonts/docs/material_symbols (Apache 2.0), Lucide https://npmjs.com/package/lucide-static (ISC)

미확인: Segoe Fluent Icons의 실제 파일명과 Ally 설치 여부, Cascadia Mono의 Windows 11 기본 포함 여부(없으면 Consolas), Pretendard 정적 TTF 용량, ImGui 1.93.0에서 `ShadeVertsLinearColorGradientKeepAlpha` 시그니처. 모두 S1에서 실기·소스로 확인한다.
