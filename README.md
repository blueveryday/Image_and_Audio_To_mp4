# Image_and_Audio_To_Mp4

基于 ffmpeg 的小工具(Windows,C++ / Win32 原生界面,单个 exe,无需安装 Python 或 VC++ 运行库)。分两个标签页:

## 标签页 1:图片 + 音频
把一张图片和一个音频文件合成为 mp4 视频(图片作为静止画面)。

## 标签页 2:视频 + 音频
把**无声的 mp4 视频**和 **mp3 / aac 等音频文件**混合在一起,生成带声音的 mp4。

可设置的参数:

| 分类 | 参数 |
| --- | --- |
| 视频 | 是否重新编码(不勾选则直接复制视频流,画质无损、速度最快)、分辨率(原分辨率 / 4K / 2K / 1080p / 720p / 480p / 360p / 自定义,可选保持宽高比并填充黑边)、帧率、视频编码器(libx264 / libx265 / h264_nvenc / hevc_nvenc)、编码速度 preset、画质 CRF 或目标码率、最大码率、像素格式 |
| 音频 | 音频编码器(aac / copy / libmp3lame)、音频码率、采样率、声道 |
| 合成 | 时长处理(以较短者为准 / 以视频为准并补静音 / 以视频为准并循环音频 / 以音频为准并循环视频)、faststart、覆盖输出、额外自定义参数 |

说明:
- 选择视频、音频文件后会用 ffprobe 显示它们的分辨率、帧率、码率和时长,方便对照设置。
- 视频文件里如果已有音轨,会被所选音频替换。
- “循环音频 / 循环视频”模式下,被循环的那一路必须重新编码(直接复制时 ffmpeg 无法按另一路的时长结束),程序会自动切换并给出提示。
- 参数填错时(如码率写成了文字)不会报错中断,而是忽略该参数并在界面上提示。
- 下方的命令行会随选项自动更新(标签页 1 点“根据以上选项生成命令”),也可以直接手动修改,运行时以文本框内容为准。

## 使用
从 [Releases](../../releases) 下载压缩包并解压,运行 `ImageAudioTomp4.exe`。压缩包里已带 `ffmpeg.exe` 和 `ffprobe.exe`(放在同一目录即可被自动找到);也可以在窗口顶部手动指定其他位置的 ffmpeg。

## 从源码构建
需要 CMake 3.15+ 和 MSVC(Visual Studio 2019 / 2022)或 MinGW-w64:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

在 Linux 上交叉编译 Windows 程序:

```
sudo apt install mingw-w64 cmake ninja-build
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win
```

GitHub Actions(`.github/workflows/build_exe.yml`)在推送到 `C` 分支时自动:Linux 上跑核心测试 → MSVC 静态链接编译 exe → 检查无 VC++ 运行库依赖 → 连同 ffmpeg 打包为 7z 并发布 Release。

## 代码结构
```
src/core.hpp          与界面无关的核心逻辑:命令拆分/转义、两个标签页的 ffmpeg 命令生成、ffprobe 输出解析
src/main.cpp          Win32 界面(两个标签页、启动 ffmpeg、读取日志、停止)
src/app.rc, app.manifest   内嵌 manifest(现代控件外观、高 DPI)
tests/core_test.cpp   核心逻辑的测试(Linux,需要 ffmpeg/ffprobe)
cmake/                MinGW-w64 交叉编译工具链文件
assets/               打包进发布版的 ffmpeg / ffprobe
```

运行测试(Linux / macOS,PATH 里需要有 ffmpeg 和 ffprobe):

```
cmake -S . -B build-test && cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

## 关于 Python 版
原来的 Python (tkinter) 版本保留在 `main` 分支。
