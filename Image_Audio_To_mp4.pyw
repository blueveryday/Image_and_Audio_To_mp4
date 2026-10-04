#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
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
VIDEO_SAVE_EXTS = [
    ("MP4 文件", "*.mp4"),
    ("所有文件", "*.*"),
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


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("图片 + 音频 合成 MP4 (ffmpeg)")
        self.geometry("880x760")
        self.minsize(820, 700)

        self.proc = None

        self._build_ui()
        self._update_command_preview()

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

        frm_cmd = ttk.LabelFrame(self, text="完整命令行(可直接编辑,运行时以此文本框内容为准)")
        frm_cmd.pack(fill="both", expand=False, **pad)

        self.txt_cmd = tk.Text(frm_cmd, height=5, wrap="word")
        self.txt_cmd.pack(fill="both", expand=True, padx=6, pady=6)

        frm_run = ttk.Frame(self)
        frm_run.pack(fill="x", **pad)

        self.btn_run = ttk.Button(frm_run, text="▶ 运行", command=self._run_ffmpeg)
        self.btn_run.pack(side="left", padx=6)

        self.btn_stop = ttk.Button(frm_run, text="■ 停止", command=self._stop_ffmpeg, state="disabled")
        self.btn_stop.pack(side="left", padx=6)

        ttk.Button(frm_run, text="清空日志", command=self._clear_log).pack(side="left", padx=6)

        frm_log = ttk.LabelFrame(self, text="日志输出")
        frm_log.pack(fill="both", expand=True, **pad)

        self.txt_log = tk.Text(frm_log, wrap="word", state="disabled", bg="#111", fg="#0f0")
        self.txt_log.pack(fill="both", expand=True, padx=6, pady=6)

    def _add_file_row(self, parent, label, var, cmd):
        frm = ttk.Frame(parent)
        frm.pack(fill="x", padx=6, pady=4)
        ttk.Label(frm, text=label, width=10).pack(side="left")
        entry = ttk.Entry(frm, textvariable=var, justify="right")
        entry.pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(frm, text="浏览...", command=cmd).pack(side="left")
        return entry

    def _choose_ffmpeg(self):
        types = [("ffmpeg.exe", "*.exe")] if os.name == "nt" else [("所有文件", "*.*")]
        path = filedialog.askopenfilename(title="选择 ffmpeg 可执行文件", filetypes=types)
        if path:
            self.var_ffmpeg.set(path)

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
        ffmpeg = self.var_ffmpeg.get().strip() or "ffmpeg"
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

        image = self.var_image.get().strip()
        audio = self.var_audio.get().strip()
        if "<图片文件>" in cmd_str or (image and not os.path.isfile(image)):
            if not messagebox.askyesno("提示", "图片文件路径可能无效,仍要继续吗?"):
                return
        if "<音频文件>" in cmd_str or (audio and not os.path.isfile(audio)):
            if not messagebox.askyesno("提示", "音频文件路径可能无效,仍要继续吗?"):
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


if __name__ == "__main__":
    app = App()
    app.mainloop()