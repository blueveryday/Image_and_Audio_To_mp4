#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import json
import shutil
import threading
import subprocess

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

IMAGE_EXTS = [
    ("图片文件", "*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.gif"),
    ("所有文件", "*.*"),
]
AUDIO_EXTS = [
    ("音频文件", "*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.wma *.opus"),
    ("所有文件", "*.*"),
]
VIDEO_IN_EXTS = [
    ("视频文件", "*.mp4 *.m4v *.mov *.mkv *.avi *.webm"),
    ("所有文件", "*.*"),
]
VIDEO_SAVE_EXTS = [
    ("MP4 文件", "*.mp4"),
    ("所有文件", "*.*"),
]

# ---- “视频 + 音频”标签页用到的选项 ----
RES_KEEP = "保持原分辨率"
RES_CUSTOM = "自定义"
RES_PRESETS = [
    RES_KEEP,
    "3840x2160 (4K)",
    "2560x1440 (2K)",
    "1920x1080 (1080p)",
    "1280x720 (720p)",
    "854x480 (480p)",
    "640x360 (360p)",
    RES_CUSTOM,
]
FPS_KEEP = "保持原帧率"
FPS_VALUES = [FPS_KEEP, "24", "25", "30", "50", "60"]
VCODEC_VALUES = ["libx264", "libx265", "h264_nvenc", "hevc_nvenc"]
PRESET_VALUES = [
    "ultrafast", "superfast", "veryfast", "faster", "fast",
    "medium", "slow", "slower", "veryslow",
]
PIXFMT_VALUES = ["yuv420p", "yuv420p10le", "yuv444p"]
ACODEC_VALUES = ["aac", "copy", "libmp3lame"]
ABITRATE_VALUES = ["96k", "128k", "160k", "192k", "256k", "320k"]
KEEP_SAME = "保持原样"
ARATE_VALUES = [KEEP_SAME, "44100", "48000"]
ACHANNEL_VALUES = [KEEP_SAME, "1 (单声道)", "2 (立体声)"]
# (界面文字, 内部标识)
DURATION_MODES = [
    ("以较短者为准", "shortest"),
    ("以视频时长为准(音频不足补静音)", "video_pad"),
    ("以视频时长为准(音频不足则循环)", "video_loop_audio"),
    ("以音频时长为准(视频不足则循环)", "audio_loop_video"),
]


def quote_arg(arg: str) -> str:
    if arg == "" or any(ch.isspace() for ch in arg):
        return '"' + arg.replace('"', '\\"') + '"'
    return arg


def split_cmd(cmd_str: str):
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|(\S+)', cmd_str)
    result = []
    for quoted, unquoted in tokens:
        if quoted or (quoted == "" and unquoted == ""):
            result.append(quoted.replace('\\"', '"'))
        else:
            result.append(unquoted)
    return result


def find_default_ffmpeg():
    candidates = []
    exe_name = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"

    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.join(script_dir, exe_name))
    candidates.append(os.path.join(os.getcwd(), exe_name))

    for c in candidates:
        if os.path.isfile(c):
            return c

    found = shutil.which(exe_name) or shutil.which("ffmpeg")
    if found:
        return found

    return os.path.join(script_dir, exe_name)


def find_ffprobe(ffmpeg_path: str):
    """优先找 ffmpeg 同目录下的 ffprobe,其次找 PATH;找不到返回 None"""
    exe_name = "ffprobe.exe" if os.name == "nt" else "ffprobe"
    folder = os.path.dirname(ffmpeg_path) if ffmpeg_path else ""
    if folder:
        cand = os.path.join(folder, exe_name)
        if os.path.isfile(cand):
            return cand
    return shutil.which(exe_name) or shutil.which("ffprobe")


def probe_media(ffprobe: str, path: str):
    """用 ffprobe 读取媒体信息,返回 dict;失败返回 None"""
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    try:
        res = subprocess.run(
            [ffprobe, "-v", "error", "-print_format", "json",
             "-show_format", "-show_streams", path],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            timeout=30,
            creationflags=creationflags,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if res.returncode != 0:
        return None
    try:
        return json.loads(res.stdout.decode("utf-8", errors="replace"))
    except ValueError:
        return None


def _to_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _parse_fps(rate):
    """'30000/1001' -> 29.97"""
    try:
        num, den = str(rate).split("/")
        den = float(den)
        return float(num) / den if den else 0.0
    except ValueError:
        return _to_float(rate)


def describe_media(info, kind: str) -> str:
    """把 ffprobe 的结果整理成一行说明。kind 为 'video' 或 'audio'"""
    if not info:
        return "(无法读取媒体信息:文件可能已损坏或格式不受支持)"

    streams = info.get("streams") or []
    fmt = info.get("format") or {}
    duration = _to_float(fmt.get("duration"))

    if kind == "video":
        v = next((s for s in streams if s.get("codec_type") == "video"), None)
        if not v:
            return "(文件中没有视频流)"
        parts = []
        if v.get("width") and v.get("height"):
            parts.append(f"{v['width']}x{v['height']}")
        fps = _parse_fps(v.get("avg_frame_rate") or v.get("r_frame_rate"))
        if fps:
            parts.append(f"{fps:.2f}".rstrip("0").rstrip(".") + " fps")
        if v.get("codec_name"):
            parts.append(v["codec_name"])
        bitrate = _to_float(v.get("bit_rate")) or _to_float(fmt.get("bit_rate"))
        if bitrate:
            parts.append(f"{bitrate / 1000:.0f} kb/s")
        if duration:
            parts.append(f"{duration:.2f} 秒")
        text = "原视频: " + " · ".join(parts)
        if any(s.get("codec_type") == "audio" for s in streams):
            text += "  (该文件含音轨,合成时会被替换)"
        return text

    a = next((s for s in streams if s.get("codec_type") == "audio"), None)
    if not a:
        return "(文件中没有音频流)"
    parts = []
    if a.get("codec_name"):
        parts.append(a["codec_name"])
    if a.get("sample_rate"):
        parts.append(f"{a['sample_rate']} Hz")
    channels = a.get("channels")
    if channels:
        parts.append({1: "单声道", 2: "立体声"}.get(channels, f"{channels} 声道"))
    bitrate = _to_float(a.get("bit_rate")) or _to_float(fmt.get("bit_rate"))
    if bitrate:
        parts.append(f"{bitrate / 1000:.0f} kb/s")
    if duration:
        parts.append(f"{duration:.2f} 秒")
    return "原音频: " + " · ".join(parts)


def parse_kbps(text: str):
    """把 '4000' / '4000k' / '5M' 解析成 kbps(整数)。纯数字按 kbps 理解;无效返回 None"""
    m = re.fullmatch(r"(\d+(?:\.\d+)?)\s*([kKmM]?)", (text or "").strip())
    if not m:
        return None
    value = float(m.group(1))
    if m.group(2).lower() == "m":
        value *= 1000
    kbps = int(round(value))
    return kbps if kbps > 0 else None


def is_posint(text: str) -> bool:
    return re.fullmatch(r"[0-9]+", text or "") is not None and int(text) > 0


# ---------------------------------------------------------------------------
# 标签页基类:命令预览 / 运行 / 停止 / 日志
# ---------------------------------------------------------------------------
class FfmpegTab(ttk.Frame):
    CMD_TITLE = "完整命令行(可直接编辑,运行时以此文本框内容为准)"
    CMD_HEIGHT = 5

    def __init__(self, master, app):
        super().__init__(master)
        self.app = app
        self.proc = None

        self._build_ui()
        self._after_build()
        self._update_command_preview()

    # ---- 子类需要实现 / 可覆盖的部分 ----
    def _build_options_ui(self):
        raise NotImplementedError

    def _build_command_list(self):
        raise NotImplementedError

    def _check_inputs(self, cmd_str: str) -> bool:
        """运行前检查输入文件,返回 False 则不运行"""
        return True

    def _after_build(self):
        """界面全部创建完之后调用(例如绑定变量监听)"""

    # ---- 公共界面 ----
    def _build_ui(self):
        pad = {"padx": 8, "pady": 4}

        self._build_options_ui()

        frm_cmd = ttk.LabelFrame(self, text=self.CMD_TITLE)
        frm_cmd.pack(fill="both", expand=False, **pad)

        self.txt_cmd = tk.Text(frm_cmd, height=self.CMD_HEIGHT, wrap="word")
        self.txt_cmd.pack(fill="both", expand=True, padx=6, pady=6)

        self.frm_run = ttk.Frame(self)
        self.frm_run.pack(fill="x", **pad)

        self.btn_run = ttk.Button(self.frm_run, text="▶ 运行", command=self._run_ffmpeg)
        self.btn_run.pack(side="left", padx=6)

        self.btn_stop = ttk.Button(self.frm_run, text="■ 停止", command=self._stop_ffmpeg, state="disabled")
        self.btn_stop.pack(side="left", padx=6)

        ttk.Button(self.frm_run, text="清空日志", command=self._clear_log).pack(side="left", padx=6)

        frm_log = ttk.LabelFrame(self, text="日志输出")
        frm_log.pack(fill="both", expand=True, **pad)

        self.txt_log = tk.Text(frm_log, wrap="word", state="disabled", bg="#111", fg="#0f0", height=6)
        self.txt_log.pack(fill="both", expand=True, padx=6, pady=6)

    def _add_file_row(self, parent, label, var, cmd):
        frm = ttk.Frame(parent)
        frm.pack(fill="x", padx=6, pady=4)
        ttk.Label(frm, text=label, width=10).pack(side="left")
        entry = ttk.Entry(frm, textvariable=var, justify="right")
        entry.pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(frm, text="浏览...", command=cmd).pack(side="left")
        return entry

    def _update_command_preview(self):
        cmd = self._build_command_list()
        cmd_str = " ".join(quote_arg(c) for c in cmd)
        self.txt_cmd.delete("1.0", "end")
        self.txt_cmd.insert("1.0", cmd_str)

    def _log(self, text):
        self.txt_log.configure(state="normal")
        self.txt_log.insert("end", text)
        self.txt_log.see("end")
        self.txt_log.configure(state="disabled")

    def _clear_log(self):
        self.txt_log.configure(state="normal")
        self.txt_log.delete("1.0", "end")
        self.txt_log.configure(state="disabled")

    def _run_ffmpeg(self):
        if self.proc is not None:
            messagebox.showwarning("提示", "已有任务正在运行,请先停止或等待完成。")
            return

        cmd_str = self.txt_cmd.get("1.0", "end").strip()
        if not cmd_str:
            messagebox.showerror("错误", "命令为空。")
            return

        try:
            cmd = split_cmd(cmd_str)
        except Exception as e:
            messagebox.showerror("命令解析失败", str(e))
            return

        ffmpeg_path = cmd[0] if cmd else ""
        if not (os.path.isfile(ffmpeg_path) or shutil.which(ffmpeg_path)):
            if not messagebox.askyesno(
                "找不到 ffmpeg",
                f"未找到 ffmpeg 可执行文件:\n{ffmpeg_path}\n\n仍然尝试运行吗?",
            ):
                return

        if not self._check_inputs(cmd_str):
            return

        self._clear_log()
        self._log("执行命令:\n" + cmd_str + "\n\n")

        self.btn_run.configure(state="disabled")
        self.btn_stop.configure(state="normal")

        threading.Thread(target=self._run_in_thread, args=(cmd,), daemon=True).start()

    def _run_in_thread(self, cmd):
        try:
            creationflags = 0
            if os.name == "nt":
                creationflags = subprocess.CREATE_NO_WINDOW

            self.proc = subprocess.Popen(
                cmd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=0,
                creationflags=creationflags,
            )
            for raw_line in self.proc.stdout:
                line = self._decode_line(raw_line)
                self.after(0, self._log, line)
            ret = self.proc.wait()
            if ret == 0:
                self.after(0, self._log, "\n[完成] ffmpeg 正常退出。\n")
            else:
                self.after(0, self._log, f"\n[结束] ffmpeg 退出码: {ret}\n")
        except FileNotFoundError:
            self.after(0, self._log, "\n[错误] 找不到 ffmpeg 可执行文件,请检查路径设置。\n")
        except Exception as e:
            self.after(0, self._log, f"\n[异常] {e}\n")
        finally:
            self.proc = None
            self.after(0, lambda: self.btn_run.configure(state="normal"))
            self.after(0, lambda: self.btn_stop.configure(state="disabled"))

    @staticmethod
    def _decode_line(raw_line: bytes) -> str:
        for enc in ("utf-8", "gbk", "mbcs"):
            try:
                return raw_line.decode(enc)
            except (UnicodeDecodeError, LookupError):
                continue
        return raw_line.decode("utf-8", errors="replace")

    def _stop_ffmpeg(self):
        if self.proc is not None:
            try:
                self.proc.terminate()
                self._log("\n[已请求停止]\n")
            except Exception as e:
                self._log(f"\n[停止失败] {e}\n")


# ---------------------------------------------------------------------------
# 标签页 1:图片 + 音频 -> MP4(原有功能)
# ---------------------------------------------------------------------------
class ImageAudioTab(FfmpegTab):
    def _build_options_ui(self):
        pad = {"padx": 8, "pady": 4}

        frm_files = ttk.LabelFrame(self, text="输入 / 输出文件")
        frm_files.pack(fill="x", **pad)

        self.var_image = tk.StringVar()
        self.var_audio = tk.StringVar()
        self.var_output = tk.StringVar()

        self.ent_audio = self._add_file_row(frm_files, "音频文件:", self.var_audio, self._choose_audio)
        self.ent_image = self._add_file_row(frm_files, "图片文件:", self.var_image, self._choose_image)
        self.ent_output = self._add_file_row(frm_files, "输出文件:", self.var_output, self._choose_output)

        frm_opts = ttk.LabelFrame(self, text="命令行参数(可自定义,仅用作生成命令的默认值)")
        frm_opts.pack(fill="x", **pad)

        row = 0

        ttk.Label(frm_opts, text="输出宽度:").grid(row=row, column=0, sticky="e", padx=6, pady=4)
        self.var_width = tk.StringVar(value="1280")
        ttk.Entry(frm_opts, textvariable=self.var_width, width=10).grid(
            row=row, column=1, sticky="w", padx=6, pady=4
        )

        ttk.Label(frm_opts, text="输出高度:").grid(row=row, column=2, sticky="e", padx=6, pady=4)
        self.var_height = tk.StringVar(value="720")
        ttk.Entry(frm_opts, textvariable=self.var_height, width=10).grid(
            row=row, column=3, sticky="w", padx=6, pady=4
        )

        self.var_use_scale_pad = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            frm_opts,
            text="使用 scale+pad 保持宽高比并填充黑边",
            variable=self.var_use_scale_pad,
        ).grid(row=row, column=4, columnspan=2, sticky="w", padx=6, pady=4)

        row += 1

        ttk.Label(frm_opts, text="视频编码器 (-c:v):").grid(row=row, column=0, sticky="e", padx=6, pady=4)
        self.var_vcodec = tk.StringVar(value="libx264")
        ttk.Entry(frm_opts, textvariable=self.var_vcodec, width=12).grid(
            row=row, column=1, sticky="w", padx=6, pady=4
        )

        ttk.Label(frm_opts, text="tune:").grid(row=row, column=2, sticky="e", padx=6, pady=4)
        self.var_tune = tk.StringVar(value="stillimage")
        ttk.Entry(frm_opts, textvariable=self.var_tune, width=12).grid(
            row=row, column=3, sticky="w", padx=6, pady=4
        )

        self.var_use_tune = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm_opts, text="启用 -tune", variable=self.var_use_tune).grid(
            row=row, column=4, sticky="w", padx=6, pady=4
        )

        row += 1

        ttk.Label(frm_opts, text="音频编码器 (-c:a):").grid(row=row, column=0, sticky="e", padx=6, pady=4)
        self.var_acodec = tk.StringVar(value="aac")
        ttk.Entry(frm_opts, textvariable=self.var_acodec, width=12).grid(
            row=row, column=1, sticky="w", padx=6, pady=4
        )

        ttk.Label(frm_opts, text="像素格式 (-pix_fmt):").grid(row=row, column=2, sticky="e", padx=6, pady=4)
        self.var_pixfmt = tk.StringVar(value="yuv420p")
        ttk.Entry(frm_opts, textvariable=self.var_pixfmt, width=12).grid(
            row=row, column=3, sticky="w", padx=6, pady=4
        )

        row += 1

        self.var_shortest = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm_opts, text="使用 -shortest(以短的一方为准)", variable=self.var_shortest).grid(
            row=row, column=0, columnspan=2, sticky="w", padx=6, pady=4
        )

        self.var_overwrite = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm_opts, text="覆盖已存在的输出文件 (-y)", variable=self.var_overwrite).grid(
            row=row, column=2, columnspan=2, sticky="w", padx=6, pady=4
        )

        row += 1

        ttk.Label(frm_opts, text="额外自定义参数:").grid(row=row, column=0, sticky="e", padx=6, pady=4)
        self.var_extra = tk.StringVar(value="")
        ttk.Entry(frm_opts, textvariable=self.var_extra).grid(
            row=row, column=1, columnspan=5, sticky="we", padx=6, pady=4
        )
        frm_opts.grid_columnconfigure(5, weight=1)

        ttk.Button(frm_opts, text="根据以上选项生成命令 →", command=self._update_command_preview).grid(
            row=row + 1, column=0, columnspan=6, sticky="we", padx=6, pady=8
        )

    def _choose_image(self):
        path = filedialog.askopenfilename(title="选择图片文件", filetypes=IMAGE_EXTS)
        if path:
            self.var_image.set(path)
            self.after_idle(lambda: self.ent_image.xview_moveto(1))
            self._auto_fill_output()
            self._update_command_preview()

    def _choose_audio(self):
        path = filedialog.askopenfilename(title="选择音频文件", filetypes=AUDIO_EXTS)
        if path:
            self.var_audio.set(path)
            self.after_idle(lambda: self.ent_audio.xview_moveto(1))
            self._auto_fill_output()
            self._update_command_preview()

    def _choose_output(self):
        path = filedialog.asksaveasfilename(
            title="选择输出文件位置", defaultextension=".mp4", filetypes=VIDEO_SAVE_EXTS
        )
        if path:
            self.var_output.set(path)
            self.after_idle(lambda: self.ent_output.xview_moveto(1))
            self._update_command_preview()

    def _auto_fill_output(self):
        """若输出路径为空,根据图片/音频文件所在目录自动填一个默认输出路径"""
        if self.var_output.get().strip():
            return
        base_dir = None
        base_name = "output"
        if self.var_audio.get():
            base_dir = os.path.dirname(self.var_audio.get())
            base_name = os.path.splitext(os.path.basename(self.var_audio.get()))[0]
        elif self.var_image.get():
            base_dir = os.path.dirname(self.var_image.get())
            base_name = os.path.splitext(os.path.basename(self.var_image.get()))[0]
        if base_dir:
            self.var_output.set(os.path.join(base_dir, base_name + ".mp4"))
            self.after_idle(lambda: self.ent_output.xview_moveto(1))

    def _build_command_list(self):
        """根据界面选项生成 ffmpeg 命令参数列表"""
        ffmpeg = self.app.var_ffmpeg.get().strip() or "ffmpeg"
        image = self.var_image.get().strip()
        audio = self.var_audio.get().strip()
        output = self.var_output.get().strip()

        cmd = [ffmpeg]
        if self.var_overwrite.get():
            cmd.append("-y")

        cmd += ["-loop", "1", "-i", image or "<图片文件>"]
        cmd += ["-i", audio or "<音频文件>"]
        cmd += ["-c:v", self.var_vcodec.get().strip() or "libx264"]

        if self.var_use_tune.get() and self.var_tune.get().strip():
            cmd += ["-tune", self.var_tune.get().strip()]

        cmd += ["-c:a", self.var_acodec.get().strip() or "aac"]

        if self.var_shortest.get():
            cmd.append("-shortest")

        cmd += ["-pix_fmt", self.var_pixfmt.get().strip() or "yuv420p"]

        width = self.var_width.get().strip() or "1280"
        height = self.var_height.get().strip() or "720"
        if self.var_use_scale_pad.get():
            vf = (
                f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
                f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2"
            )
            cmd += ["-vf", vf]

        extra = self.var_extra.get().strip()
        if extra:
            cmd += split_cmd(extra)

        cmd.append(output or "<输出文件>")
        return cmd

    def _check_inputs(self, cmd_str):
        image = self.var_image.get().strip()
        audio = self.var_audio.get().strip()
        if "<图片文件>" in cmd_str or (image and not os.path.isfile(image)):
            if not messagebox.askyesno("提示", "图片文件路径可能无效,仍要继续吗?"):
                return False
        if "<音频文件>" in cmd_str or (audio and not os.path.isfile(audio)):
            if not messagebox.askyesno("提示", "音频文件路径可能无效,仍要继续吗?"):
                return False
        return True


# ---------------------------------------------------------------------------
# 标签页 2:无声视频 + 音频(mp3 / aac ...) -> 带声音的 MP4
# ---------------------------------------------------------------------------
class VideoAudioTab(FfmpegTab):
    CMD_TITLE = "完整命令行(随上方选项自动更新;也可直接编辑,运行时以此文本框内容为准)"
    CMD_HEIGHT = 3

    def __init__(self, master, app):
        self._notes = []
        self._auto_output = ""  # 上一次自动填入的输出路径,用来判断用户有没有手动改过
        super().__init__(master, app)

    # ---- 界面 ----
    def _build_options_ui(self):
        pad = {"padx": 8, "pady": 4}
        g = {"padx": 4, "pady": 2}

        # 文件
        frm_files = ttk.LabelFrame(self, text="输入 / 输出文件(视频为无声 mp4,音频为 mp3 / aac 等)")
        frm_files.pack(fill="x", **pad)

        self.var_video = tk.StringVar()
        self.var_audio = tk.StringVar()
        self.var_output = tk.StringVar()
        self.var_vinfo = tk.StringVar()
        self.var_ainfo = tk.StringVar()

        self.ent_video = self._add_file_row(frm_files, "视频文件:", self.var_video, self._choose_video)
        self.ent_audio = self._add_file_row(frm_files, "音频文件:", self.var_audio, self._choose_audio)
        self.ent_output = self._add_file_row(frm_files, "输出文件:", self.var_output, self._choose_output)
        for var in (self.var_vinfo, self.var_ainfo):
            ttk.Label(
                frm_files, textvariable=var, foreground="#666", wraplength=860, justify="left"
            ).pack(fill="x", padx=12, pady=(0, 2))

        # 参数区:左 视频,右 音频 / 合成
        frm_params = ttk.Frame(self)
        frm_params.pack(fill="x", **pad)
        frm_params.grid_columnconfigure(0, weight=3)
        frm_params.grid_columnconfigure(1, weight=2)

        frm_v = ttk.LabelFrame(frm_params, text="视频参数")
        frm_v.grid(row=0, column=0, sticky="nsew", padx=(0, 4))
        frm_a = ttk.LabelFrame(frm_params, text="音频 / 合成参数")
        frm_a.grid(row=0, column=1, sticky="nsew", padx=(4, 0))

        # -- 视频参数 --
        r = 0
        self.var_reencode = tk.BooleanVar(value=True)
        self.chk_reencode = ttk.Checkbutton(
            frm_v, text="重新编码视频(取消勾选则直接复制视频流,画质无损、速度快)",
            variable=self.var_reencode,
        )
        self.chk_reencode.grid(row=r, column=0, columnspan=4, sticky="w", **g)

        r += 1
        ttk.Label(frm_v, text="分辨率:").grid(row=r, column=0, sticky="e", **g)
        self.var_res = tk.StringVar(value=RES_KEEP)
        self.cmb_res = ttk.Combobox(
            frm_v, textvariable=self.var_res, values=RES_PRESETS, state="readonly", width=16
        )
        self.cmb_res.grid(row=r, column=1, sticky="w", **g)
        frm_wh = ttk.Frame(frm_v)
        frm_wh.grid(row=r, column=2, columnspan=2, sticky="w", **g)
        self.var_width = tk.StringVar(value="1920")
        self.var_height = tk.StringVar(value="1080")
        ttk.Label(frm_wh, text="宽:").pack(side="left")
        self.ent_w = ttk.Entry(frm_wh, textvariable=self.var_width, width=6)
        self.ent_w.pack(side="left", padx=(2, 8))
        ttk.Label(frm_wh, text="高:").pack(side="left")
        self.ent_h = ttk.Entry(frm_wh, textvariable=self.var_height, width=6)
        self.ent_h.pack(side="left", padx=2)

        r += 1
        self.var_pad = tk.BooleanVar(value=True)
        self.chk_pad = ttk.Checkbutton(
            frm_v, text="保持宽高比并填充黑边", variable=self.var_pad
        )
        self.chk_pad.grid(row=r, column=0, columnspan=2, sticky="w", **g)
        ttk.Label(frm_v, text="帧率 (fps):").grid(row=r, column=2, sticky="e", **g)
        self.var_fps = tk.StringVar(value=FPS_KEEP)
        self.cmb_fps = ttk.Combobox(frm_v, textvariable=self.var_fps, values=FPS_VALUES, width=8)
        self.cmb_fps.grid(row=r, column=3, sticky="w", **g)

        r += 1
        ttk.Label(frm_v, text="视频编码器:").grid(row=r, column=0, sticky="e", **g)
        self.var_vcodec = tk.StringVar(value="libx264")
        self.cmb_vcodec = ttk.Combobox(frm_v, textvariable=self.var_vcodec, values=VCODEC_VALUES, width=12)
        self.cmb_vcodec.grid(row=r, column=1, sticky="w", **g)
        ttk.Label(frm_v, text="编码速度:").grid(row=r, column=2, sticky="e", **g)
        self.var_preset = tk.StringVar(value="medium")
        self.cmb_preset = ttk.Combobox(
            frm_v, textvariable=self.var_preset, values=PRESET_VALUES, state="readonly", width=9
        )
        self.cmb_preset.grid(row=r, column=3, sticky="w", **g)

        r += 1
        self.var_rc = tk.StringVar(value="crf")
        self.rad_crf = ttk.Radiobutton(frm_v, text="恒定质量 CRF:", variable=self.var_rc, value="crf")
        self.rad_crf.grid(row=r, column=0, sticky="w", **g)
        self.var_crf = tk.StringVar(value="23")
        self.spn_crf = ttk.Spinbox(frm_v, from_=0, to=51, textvariable=self.var_crf, width=6)
        self.spn_crf.grid(row=r, column=1, sticky="w", **g)
        self.rad_abr = ttk.Radiobutton(frm_v, text="目标码率(kbps):", variable=self.var_rc, value="abr")
        self.rad_abr.grid(row=r, column=2, sticky="w", **g)
        self.var_vbr = tk.StringVar(value="4000")
        self.ent_vbr = ttk.Entry(frm_v, textvariable=self.var_vbr, width=9)
        self.ent_vbr.grid(row=r, column=3, sticky="w", **g)

        r += 1
        ttk.Label(frm_v, text="最大码率(kbps):").grid(row=r, column=0, sticky="e", **g)
        self.var_maxrate = tk.StringVar(value="")
        self.ent_maxrate = ttk.Entry(frm_v, textvariable=self.var_maxrate, width=10)
        self.ent_maxrate.grid(row=r, column=1, sticky="w", **g)
        ttk.Label(frm_v, text="像素格式:").grid(row=r, column=2, sticky="e", **g)
        self.var_pixfmt = tk.StringVar(value="yuv420p")
        self.cmb_pixfmt = ttk.Combobox(frm_v, textvariable=self.var_pixfmt, values=PIXFMT_VALUES, width=10)
        self.cmb_pixfmt.grid(row=r, column=3, sticky="w", **g)

        r += 1
        ttk.Label(
            frm_v,
            text="CRF 越小画质越高(常用 18~28);码率单位 kbps,可写 4000 或 5M",
            foreground="#666", wraplength=540, justify="left",
        ).grid(row=r, column=0, columnspan=4, sticky="w", **g)

        # -- 音频 / 合成参数 --
        r = 0
        ttk.Label(frm_a, text="音频编码器:").grid(row=r, column=0, sticky="e", **g)
        self.var_acodec = tk.StringVar(value="aac")
        self.cmb_acodec = ttk.Combobox(frm_a, textvariable=self.var_acodec, values=ACODEC_VALUES, width=11)
        self.cmb_acodec.grid(row=r, column=1, sticky="w", **g)
        ttk.Label(frm_a, text="音频码率:").grid(row=r, column=2, sticky="e", **g)
        self.var_abitrate = tk.StringVar(value="192k")
        self.cmb_abitrate = ttk.Combobox(frm_a, textvariable=self.var_abitrate, values=ABITRATE_VALUES, width=7)
        self.cmb_abitrate.grid(row=r, column=3, sticky="w", **g)

        r += 1
        ttk.Label(frm_a, text="采样率 (Hz):").grid(row=r, column=0, sticky="e", **g)
        self.var_arate = tk.StringVar(value=KEEP_SAME)
        self.cmb_arate = ttk.Combobox(frm_a, textvariable=self.var_arate, values=ARATE_VALUES, width=11)
        self.cmb_arate.grid(row=r, column=1, sticky="w", **g)
        ttk.Label(frm_a, text="声道:").grid(row=r, column=2, sticky="e", **g)
        self.var_achannels = tk.StringVar(value=KEEP_SAME)
        self.cmb_achannels = ttk.Combobox(
            frm_a, textvariable=self.var_achannels, values=ACHANNEL_VALUES, state="readonly", width=9
        )
        self.cmb_achannels.grid(row=r, column=3, sticky="w", **g)

        r += 1
        ttk.Label(frm_a, text="时长处理:").grid(row=r, column=0, sticky="e", **g)
        self.var_duration = tk.StringVar(value=DURATION_MODES[0][0])
        self.cmb_duration = ttk.Combobox(
            frm_a, textvariable=self.var_duration, values=[t for t, _ in DURATION_MODES],
            state="readonly", width=28,
        )
        self.cmb_duration.grid(row=r, column=1, columnspan=3, sticky="we", **g)

        r += 1
        self.var_faststart = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            frm_a, text="faststart(便于网页边下边播)", variable=self.var_faststart
        ).grid(row=r, column=0, columnspan=2, sticky="w", **g)
        self.var_overwrite = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            frm_a, text="覆盖输出文件 (-y)", variable=self.var_overwrite
        ).grid(row=r, column=2, columnspan=2, sticky="w", **g)

        r += 1
        ttk.Label(frm_a, text="额外参数:").grid(row=r, column=0, sticky="e", **g)
        self.var_extra = tk.StringVar(value="")
        ttk.Entry(frm_a, textvariable=self.var_extra).grid(
            row=r, column=1, columnspan=3, sticky="we", **g
        )
        frm_a.grid_columnconfigure(3, weight=1)

        # 提示信息(参数冲突、被忽略的参数等),显示在“运行”按钮同一行,见 _after_build
        self.var_note = tk.StringVar()

    def _after_build(self):
        ttk.Button(
            self.frm_run, text="↻ 按选项重新生成命令", command=self._update_command_preview
        ).pack(side="left", padx=6)
        ttk.Label(
            self.frm_run, textvariable=self.var_note, foreground="#b45309", wraplength=440, justify="left"
        ).pack(side="left", fill="x", expand=True, padx=(10, 6))

        # 选了分辨率预设就把宽高填进去(要先于通用监听注册)
        self.var_res.trace_add("write", self._on_res_changed)
        watched = [
            self.app.var_ffmpeg,
            self.var_video, self.var_audio, self.var_output,
            self.var_reencode, self.var_res, self.var_width, self.var_height, self.var_pad,
            self.var_fps, self.var_vcodec, self.var_preset, self.var_rc, self.var_crf,
            self.var_vbr, self.var_maxrate, self.var_pixfmt,
            self.var_acodec, self.var_abitrate, self.var_arate, self.var_achannels,
            self.var_duration, self.var_faststart, self.var_overwrite, self.var_extra,
        ]
        for var in watched:
            var.trace_add("write", self._on_option_changed)

        # 手动粘贴/输入路径后,离开输入框或按回车时读取媒体信息
        for ent, kind in ((self.ent_video, "video"), (self.ent_audio, "audio")):
            ent.bind("<FocusOut>", lambda _e, k=kind: self._probe_async(k))
            ent.bind("<Return>", lambda _e, k=kind: self._probe_async(k))

        self._sync_states()

    # ---- 选项联动 ----
    def _on_res_changed(self, *_):
        m = re.match(r"(\d+)x(\d+)", self.var_res.get())
        if m:
            self.var_width.set(m.group(1))
            self.var_height.set(m.group(2))

    def _on_option_changed(self, *_):
        self._sync_states()
        self._update_command_preview()

    def _sync_states(self):
        def st(enabled, normal="normal"):
            return normal if enabled else "disabled"

        mode = self._duration_mode()
        reenc = self._effective_reencode(mode)
        keep_res = self.var_res.get() == RES_KEEP
        crf_mode = self.var_rc.get() == "crf"

        self.cmb_res.configure(state=st(reenc, "readonly"))
        for w in (self.ent_w, self.ent_h, self.chk_pad):
            w.configure(state=st(reenc and not keep_res))
        self.cmb_preset.configure(state=st(reenc, "readonly"))
        for w in (self.cmb_fps, self.cmb_vcodec, self.cmb_pixfmt, self.ent_maxrate,
                  self.rad_crf, self.rad_abr):
            w.configure(state=st(reenc))
        self.spn_crf.configure(state=st(reenc and crf_mode))
        self.ent_vbr.configure(state=st(reenc and not crf_mode))

        acopy = self._effective_acodec(mode) == "copy"
        self.cmb_abitrate.configure(state=st(not acopy))
        self.cmb_arate.configure(state=st(not acopy))
        self.cmb_achannels.configure(state=st(not acopy, "readonly"))

    # ---- 文件选择 ----
    def _choose_video(self):
        path = filedialog.askopenfilename(title="选择无声视频文件", filetypes=VIDEO_IN_EXTS)
        if path:
            self.var_video.set(path)
            self.after_idle(lambda: self.ent_video.xview_moveto(1))
            self._auto_fill_output()
            self._probe_async("video")

    def _choose_audio(self):
        path = filedialog.askopenfilename(title="选择音频文件(mp3 / aac 等)", filetypes=AUDIO_EXTS)
        if path:
            self.var_audio.set(path)
            self.after_idle(lambda: self.ent_audio.xview_moveto(1))
            self._probe_async("audio")

    def _choose_output(self):
        path = filedialog.asksaveasfilename(
            title="选择输出文件位置", defaultextension=".mp4", filetypes=VIDEO_SAVE_EXTS
        )
        if path:
            self.var_output.set(path)
            self.after_idle(lambda: self.ent_output.xview_moveto(1))

    def _auto_fill_output(self):
        """输出为空(或仍是上次自动填的值)时,按视频文件名生成 <视频名>_with_audio.mp4"""
        current = self.var_output.get().strip()
        if current and current != self._auto_output:
            return
        video = self.var_video.get().strip()
        if not video:
            return
        base = os.path.splitext(os.path.basename(video))[0]
        self._auto_output = os.path.join(os.path.dirname(video), base + "_with_audio.mp4")
        self.var_output.set(self._auto_output)
        self.after_idle(lambda: self.ent_output.xview_moveto(1))

    # ---- 读取输入文件信息(ffprobe) ----
    def _probe_async(self, kind):
        var_path = self.var_video if kind == "video" else self.var_audio
        var_info = self.var_vinfo if kind == "video" else self.var_ainfo
        path = var_path.get().strip()
        if not path or not os.path.isfile(path):
            var_info.set("")
            return

        ffprobe = find_ffprobe(self.app.var_ffmpeg.get().strip())
        if not ffprobe:
            var_info.set("(未找到 ffprobe,无法显示媒体信息,不影响合成)")
            return

        var_info.set("正在读取媒体信息…")

        def work():
            text = describe_media(probe_media(ffprobe, path), kind)

            def apply():
                if var_path.get().strip() == path:  # 期间没换文件才更新
                    var_info.set(text)

            self.after(0, apply)

        threading.Thread(target=work, daemon=True).start()

    # ---- 生成命令 ----
    def _duration_mode(self):
        text = self.var_duration.get()
        for label, mode in DURATION_MODES:
            if label == text:
                return mode
        return "shortest"

    # 注意:被循环(-stream_loop)的那一路如果是直接复制(copy),-shortest 不会生效,
    # ffmpeg 会一直写下去,所以循环模式下被循环的那一路必须重新编码。
    def _effective_reencode(self, mode):
        return self.var_reencode.get() or mode == "audio_loop_video"

    def _effective_acodec(self, mode):
        acodec = self.var_acodec.get().strip() or "aac"
        if acodec == "copy" and mode in ("video_pad", "video_loop_audio"):
            return "aac"
        return acodec

    def _video_args(self):
        """重新编码时的视频参数"""
        args = []
        note = self._notes.append

        vcodec = self.var_vcodec.get().strip() or "libx264"
        is_x26x = vcodec in ("libx264", "libx265")
        is_nvenc = vcodec.endswith("_nvenc")
        args += ["-c:v", vcodec]

        preset = self.var_preset.get().strip()
        if is_x26x and preset:
            args += ["-preset", preset]

        # 画质 / 码率
        if self.var_rc.get() == "crf":
            crf = self.var_crf.get().strip()
            if not re.fullmatch(r"[0-9]{1,2}", crf):
                if crf:
                    note("CRF 应为 0~51 的整数,已改用 23")
                crf = "23"
            if is_x26x:
                args += ["-crf", crf]
            elif is_nvenc:
                args += ["-rc", "vbr", "-cq", crf, "-b:v", "0"]
            else:
                note(f"编码器 {vcodec} 未适配 CRF,未添加画质参数,请改用目标码率或在额外参数中指定")
        else:
            kbps = parse_kbps(self.var_vbr.get())
            if kbps:
                args += ["-b:v", f"{kbps}k"]
            else:
                note("目标码率无效,已忽略")

        maxrate_text = self.var_maxrate.get().strip()
        if maxrate_text:
            maxrate = parse_kbps(maxrate_text)
            if maxrate:
                args += ["-maxrate", f"{maxrate}k", "-bufsize", f"{maxrate * 2}k"]
            else:
                note("最大码率无效,已忽略")

        pixfmt = self.var_pixfmt.get().strip()
        if pixfmt:
            args += ["-pix_fmt", pixfmt]

        # 分辨率
        if self.var_res.get() != RES_KEEP:
            w = self.var_width.get().strip()
            h = self.var_height.get().strip()
            if is_posint(w) and is_posint(h):
                if int(w) % 2 or int(h) % 2:
                    note("宽、高建议取偶数(yuv420p 要求),否则 ffmpeg 可能报错")
                if self.var_pad.get():
                    vf = (
                        f"scale={w}:{h}:force_original_aspect_ratio=decrease,"
                        f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,setsar=1"
                    )
                else:
                    vf = f"scale={w}:{h},setsar=1"
                args += ["-vf", vf]
            else:
                note("分辨率的宽、高必须是正整数,已保持原分辨率")

        # 帧率
        fps = self.var_fps.get().strip()
        if fps and fps != FPS_KEEP:
            if re.fullmatch(r"[0-9]+(?:\.[0-9]+)?(?:/[0-9]+)?", fps):
                args += ["-r", fps]
            else:
                note("帧率格式无效(可写 30、29.97 或 30000/1001),已保持原帧率")

        if vcodec in ("libx265", "hevc_nvenc"):
            args += ["-tag:v", "hvc1"]  # 让 Apple 设备/QuickTime 能识别 HEVC

        return args

    def _audio_args(self, mode, audio_path):
        args = []
        note = self._notes.append

        acodec = self._effective_acodec(mode)
        if acodec != (self.var_acodec.get().strip() or "aac"):
            if mode == "video_pad":
                note("“音频不足补静音”需要重新编码音频,已改用 aac")
            else:
                note("循环音频需要重新编码音频,已改用 aac(直接复制时 ffmpeg 无法按视频时长结束)")

        if acodec == "copy":
            args += ["-c:a", "copy"]
            ext = os.path.splitext(audio_path)[1].lower()
            if ext and ext not in (".mp3", ".aac", ".m4a", ".mp4"):
                note(f"{ext} 音频直接复制进 MP4 可能不被支持,建议音频编码器选 aac")
            return args

        args += ["-c:a", acodec]

        abitrate_text = self.var_abitrate.get().strip()
        if abitrate_text:
            kbps = parse_kbps(abitrate_text)
            if kbps:
                args += ["-b:a", f"{kbps}k"]
            else:
                note("音频码率无效,已忽略")

        arate = self.var_arate.get().strip()
        if arate and arate != KEEP_SAME:
            if is_posint(arate):
                args += ["-ar", arate]
            else:
                note("采样率必须是正整数,已保持原样")

        channels = self.var_achannels.get().strip()
        if channels and channels != KEEP_SAME:
            args += ["-ac", channels.split()[0]]

        return args

    def _build_command_list(self):
        """根据界面选项生成 ffmpeg 命令参数列表"""
        self._notes = []
        ffmpeg = self.app.var_ffmpeg.get().strip() or "ffmpeg"
        video = self.var_video.get().strip()
        audio = self.var_audio.get().strip()
        output = self.var_output.get().strip()
        mode = self._duration_mode()

        cmd = [ffmpeg]
        if self.var_overwrite.get():
            cmd.append("-y")

        # 循环输入要写在对应的 -i 之前
        if mode == "audio_loop_video":
            cmd += ["-stream_loop", "-1"]
        cmd += ["-i", video or "<视频文件>"]
        if mode == "video_loop_audio":
            cmd += ["-stream_loop", "-1"]
        cmd += ["-i", audio or "<音频文件>"]

        # 明确只取视频文件的画面、音频文件的声音
        cmd += ["-map", "0:v:0", "-map", "1:a:0"]

        if self._effective_reencode(mode):
            if not self.var_reencode.get():
                self._notes.append("循环视频需要重新编码视频,已自动启用重新编码(直接复制时 ffmpeg 无法按音频时长结束)")
            cmd += self._video_args()
        else:
            cmd += ["-c:v", "copy"]

        cmd += self._audio_args(mode, audio)
        if mode == "video_pad":
            cmd += ["-af", "apad"]

        cmd.append("-shortest")

        if self.var_faststart.get():
            cmd += ["-movflags", "+faststart"]

        extra = self.var_extra.get().strip()
        if extra:
            cmd += split_cmd(extra)

        cmd.append(output or "<输出文件>")
        return cmd

    def _update_command_preview(self):
        super()._update_command_preview()
        self.var_note.set("提示:" + ";".join(self._notes) if self._notes else "")

    # ---- 运行前检查 ----
    def _check_inputs(self, cmd_str):
        video = self.var_video.get().strip()
        audio = self.var_audio.get().strip()
        output = self.var_output.get().strip()

        if "<视频文件>" in cmd_str or (video and not os.path.isfile(video)):
            if not messagebox.askyesno("提示", "视频文件路径可能无效,仍要继续吗?"):
                return False
        if "<音频文件>" in cmd_str or (audio and not os.path.isfile(audio)):
            if not messagebox.askyesno("提示", "音频文件路径可能无效,仍要继续吗?"):
                return False

        if not output or "<输出文件>" in cmd_str:
            messagebox.showerror("错误", "请先设置输出文件。")
            return False

        def norm(p):
            return os.path.normcase(os.path.abspath(p))

        if norm(output) in {norm(p) for p in (video, audio) if p}:
            messagebox.showerror("错误", "输出文件不能与输入文件相同,请换一个文件名。")
            return False
        return True


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("图片 / 视频 + 音频 合成 MP4 (ffmpeg)")
        self.geometry("1000x790")
        self.minsize(980, 700)

        self._build_ui()

    def _build_ui(self):
        pad = {"padx": 8, "pady": 4}

        frm_ffmpeg = ttk.LabelFrame(self, text="ffmpeg 可执行文件路径")
        frm_ffmpeg.pack(fill="x", **pad)

        self.var_ffmpeg = tk.StringVar(value=find_default_ffmpeg())
        ent_ffmpeg = ttk.Entry(frm_ffmpeg, textvariable=self.var_ffmpeg, justify="right")
        ent_ffmpeg.pack(side="left", fill="x", expand=True, padx=6, pady=6)
        ttk.Button(frm_ffmpeg, text="浏览...", command=self._choose_ffmpeg).pack(
            side="left", padx=6, pady=6
        )

        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True, padx=8, pady=4)

        self.tab_image = ImageAudioTab(self.notebook, self)
        self.tab_video = VideoAudioTab(self.notebook, self)
        self.notebook.add(self.tab_image, text="图片 + 音频")
        self.notebook.add(self.tab_video, text="视频 + 音频")

    def _choose_ffmpeg(self):
        types = [("ffmpeg.exe", "*.exe")] if os.name == "nt" else [("所有文件", "*.*")]
        path = filedialog.askopenfilename(title="选择 ffmpeg 可执行文件", filetypes=types)
        if path:
            self.var_ffmpeg.set(path)


if __name__ == "__main__":
    app = App()
    app.mainloop()
