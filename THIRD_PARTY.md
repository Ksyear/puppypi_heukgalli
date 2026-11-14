# Third-party notices

| Component | Version | License | Where | Use |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0 | MIT | `third_party/imgui/` (core + Win32/DX11 backends, unmodified) | UI |
| [stb_image](https://github.com/nothings/stb) | 2.32 | MIT or Public Domain (Unlicense), author's choice | `third_party/stb_image.h` (unmodified, SHA-256 `deb4f18c…6e91`) | JPEG decode |
| Malgun Gothic, Segoe UI Symbol | system fonts | Microsoft Windows license | loaded at run time from `%WINDIR%\Fonts` | text (not redistributed) |

Not included: Hiwonder PuppyPi software and the team's robot or VR code. The app is a network client only. It talks to the robot through its published interfaces (UDP Protocol v2, rosbridge, HTTP).

The test helper `tools/mock_robot.py` imports the protocol modules of the robot repository (https://github.com/Ksyear/PuppyPI_Cpp_ROS2, `ros2_ws/src/puppy_vr_bridge`) at run time from a path you pass in. It does not copy or redistribute that code.

Toolchain (not redistributed): WinLibs MinGW-w64 GCC 16.2 UCRT (GPL with the GCC runtime library exception). Statically linked binaries are not covered by the GPL.
