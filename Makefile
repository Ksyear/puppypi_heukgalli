# puppypi_heukgalli 빌드
#   흑갈리(Windows, 폴더 안 MinGW):  .\build.ps1            → build/puppypi_heukgalli.exe
#   맥/리눅스(코어 시험):            make test, make console
#   맥에서 Windows exe 교차 빌드:     make CROSS=x86_64-w64-mingw32- app

CROSS ?=
CXX := $(CROSS)g++
ifeq ($(CROSS),)
  ifeq ($(shell uname -s 2>/dev/null),Darwin)
    CXX := clang++
  endif
endif

BUILD ?= build
DEBUG ?= 0
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter -MMD -MP
ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g
  STRIP :=
else
  CXXFLAGS += -O2
  STRIP := -s
endif

# Windows 대상 여부(교차 빌드이거나 흑갈리에서 직접 빌드)
ifneq ($(findstring mingw,$(CROSS)),)
  WINDOWS := 1
endif
ifeq ($(OS),Windows_NT)
  WINDOWS := 1
endif

CORE := json util sock net rosbridge v2client discovery path_tools robot
CORE_OBJ := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(CORE)))

ifeq ($(WINDOWS),1)
  EXE := .exe
  SYS_LIBS := -lws2_32 -liphlpapi -lssp
  STATIC := -static -static-libgcc -static-libstdc++
  # 보안: 스택 보호, 버퍼 함수 검사, 형식 문자열 검사, CET 표시, ASLR(64비트 고엔트로피)·DEP
  HARDEN := -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security -fcf-protection=full
  CXXFLAGS += $(HARDEN)
  STATIC += -Wl,--dynamicbase,--nxcompat,--high-entropy-va
else
  EXE :=
  SYS_LIBS := -lpthread
  STATIC :=
endif

IMGUI := imgui imgui_draw imgui_tables imgui_widgets imgui_impl_win32 imgui_impl_dx11
IMGUI_OBJ := $(addprefix $(BUILD)/imgui/,$(addsuffix .o,$(IMGUI)))
WIN := app_main gamepad tilt textures ui theme widgets
WIN_OBJ := $(addprefix $(BUILD)/win/,$(addsuffix .o,$(WIN)))
APP_LIBS := -ld3d11 -ld3dcompiler -ldxgi -lxinput1_4 -lsensorsapi -lole32 -loleaut32 -luuid -lpropsys \
            -luser32 -lgdi32 -ldwmapi -limm32 -lwinmm -lshell32 -mwindows

.PHONY: all app test console clean
all: test console

$(BUILD)/core/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

IMGUI_CFG := -DIMGUI_USER_CONFIG='"hg_imconfig.h"' -Isrc/win

$(BUILD)/imgui/%.o: third_party/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -MMD -MP $(HARDEN) $(IMGUI_CFG) -Ithird_party/imgui -c $< -o $@

$(BUILD)/win/%.o: src/win/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_CFG) -DUNICODE -D_UNICODE -Isrc -Ithird_party/imgui -Ithird_party -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/cli/%.o: cli/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

$(BUILD)/test_core$(EXE): $(BUILD)/tests/test_core.o $(CORE_OBJ)
	$(CXX) $^ -o $@ $(STATIC) $(SYS_LIBS)

$(BUILD)/console$(EXE): $(BUILD)/cli/console.o $(CORE_OBJ)
	$(CXX) $^ -o $@ $(STATIC) $(SYS_LIBS)

$(BUILD)/puppypi_heukgalli$(EXE): $(WIN_OBJ) $(IMGUI_OBJ) $(CORE_OBJ) $(BUILD)/win/app.res.o
	$(CXX) $^ -o $@ $(STATIC) $(STRIP) $(APP_LIBS) $(SYS_LIBS)

$(BUILD)/win/app.res.o: src/win/app.rc src/win/app.manifest
	@mkdir -p $(dir $@)
	$(CROSS)windres $< -O coff -o $@

test: $(BUILD)/test_core$(EXE)
	./$(BUILD)/test_core$(EXE)

console: $(BUILD)/console$(EXE)

app: $(BUILD)/puppypi_heukgalli$(EXE)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*/*.d)
