cmake_minimum_required(VERSION 3.15)
project(ImageAudioTomp4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  # ---- Windows 程序(MSVC 或 MinGW-w64)----
  enable_language(RC)

  add_executable(ImageAudioTomp4 WIN32 src/main.cpp src/app.rc)
  target_include_directories(ImageAudioTomp4 PRIVATE src)
  set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
  target_compile_definitions(ImageAudioTomp4 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(ImageAudioTomp4 PRIVATE comctl32 comdlg32 user32 gdi32)

  if(MSVC)
    # 源码为 UTF-8;静态链接 CRT,发布后无需安装 VC++ 运行库;manifest 由 app.rc 内嵌
    target_compile_options(ImageAudioTomp4 PRIVATE /utf-8 /W4 /permissive- /EHsc)
    set_property(TARGET ImageAudioTomp4 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_link_options(ImageAudioTomp4 PRIVATE /MANIFEST:NO)
  else()
    target_compile_options(ImageAudioTomp4 PRIVATE -Wall -Wextra)
    target_link_options(ImageAudioTomp4 PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++)
  endif()
else()
  # ---- Linux / macOS:只构建核心逻辑的测试(需要 PATH 里有 ffmpeg / ffprobe)----
  enable_testing()
  add_executable(core_test tests/core_test.cpp)
  target_compile_options(core_test PRIVATE -Wall -Wextra -Wno-unused-result)
  add_test(NAME core_unit COMMAND core_test --unit)
  add_test(NAME core_integration COMMAND core_test)
endif()
