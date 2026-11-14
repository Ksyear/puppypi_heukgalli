// ImGui 설정(외부 코드는 그대로 두고 -DIMGUI_USER_CONFIG 로 넣는다): 검사 실패를 app_log.txt 에 남기고 멈춘다
#pragma once
void hg_imgui_assert_failed(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : hg_imgui_assert_failed(#_EXPR, __FILE__, __LINE__))
