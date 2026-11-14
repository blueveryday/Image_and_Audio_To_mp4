// core.hpp —— 与界面无关的核心逻辑(命令拆分/拼接、ffmpeg 命令生成、ffprobe 输出解析)。
// 只依赖标准库,因此 Windows 界面(main.cpp)和 Linux 下的单元测试共用同一份代码。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace core {

using std::string;
using std::vector;
using std::wstring;

// ---------------------------------------------------------------------------
// 界面与测试共用的常量
// ---------------------------------------------------------------------------
inline const wchar_t* const KEEP_SAME = L"保持原样";
inline const wchar_t* const FPS_KEEP = L"保持原帧率";
inline const wchar_t* const PH_IMAGE = L"<图片文件>";
inline const wchar_t* const PH_AUDIO = L"<音频文件>";
inline const wchar_t* const PH_VIDEO = L"<视频文件>";
inline const wchar_t* const PH_OUTPUT = L"<输出文件>";

struct ResPreset {
    const wchar_t* label;
    int w;
    int h;  // w == 0 表示“保持原分辨率”或“自定义”
};
// 第 0 项为“保持原分辨率”,最后一项为“自定义”
inline const vector<ResPreset>& res_presets() {
    static const vector<ResPreset> v = {
        {L"保持原分辨率", 0, 0},
        {L"3840x2160 (4K)", 3840, 2160},
        {L"2560x1440 (2K)", 2560, 1440},
        {L"1920x1080 (1080p)", 1920, 1080},
        {L"1280x720 (720p)", 1280, 720},
        {L"854x480 (480p)", 854, 480},
        {L"640x360 (360p)", 640, 360},
        {L"自定义", 0, 0},
    };
    return v;
}
inline const vector<const wchar_t*>& fps_values() {
    static const vector<const wchar_t*> v = {FPS_KEEP, L"24", L"25", L"30", L"50", L"60"};
    return v;
}
inline const vector<const wchar_t*>& vcodec_values() {
    static const vector<const wchar_t*> v = {L"libx264", L"libx265", L"h264_nvenc", L"hevc_nvenc"};
    return v;
}
inline const vector<const wchar_t*>& preset_values() {
    static const vector<const wchar_t*> v = {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast",
                                             L"medium",    L"slow",      L"slower",   L"veryslow"};
    return v;
}
inline const vector<const wchar_t*>& pixfmt_values() {
    static const vector<const wchar_t*> v = {L"yuv420p", L"yuv420p10le", L"yuv444p"};
    return v;
}
inline const vector<const wchar_t*>& acodec_values() {
    static const vector<const wchar_t*> v = {L"aac", L"copy", L"libmp3lame"};
    return v;
}
inline const vector<const wchar_t*>& abitrate_values() {
    static const vector<const wchar_t*> v = {L"96k", L"128k", L"160k", L"192k", L"256k", L"320k"};
    return v;
}
inline const vector<const wchar_t*>& arate_values() {
    static const vector<const wchar_t*> v = {KEEP_SAME, L"44100", L"48000"};
    return v;
}
inline const vector<const wchar_t*>& achannel_values() {
    static const vector<const wchar_t*> v = {KEEP_SAME, L"1 (单声道)", L"2 (立体声)"};
    return v;
}

enum class DurMode { Shortest, VideoPad, VideoLoopAudio, AudioLoopVideo };
struct DurItem {
    const wchar_t* label;
    DurMode mode;
};
inline const vector<DurItem>& duration_modes() {
    static const vector<DurItem> v = {
        {L"以较短者为准", DurMode::Shortest},
        {L"以视频时长为准(音频不足补静音)", DurMode::VideoPad},
        {L"以视频时长为准(音频不足则循环)", DurMode::VideoLoopAudio},
        {L"以音频时长为准(视频不足则循环)", DurMode::AudioLoopVideo},
    };
    return v;
}

// ---------------------------------------------------------------------------
// 字符串小工具
// ---------------------------------------------------------------------------
inline bool is_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v' || c == 0x3000;
}

inline wstring trim(const wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline wstring lower(wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

inline bool is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// 全是 0-9 且数值 > 0
inline bool is_posint(const wstring& s) {
    if (s.empty()) return false;
    bool nonzero = false;
    for (wchar_t c : s) {
        if (!is_digit(c)) return false;
        if (c != L'0') nonzero = true;
    }
    return nonzero;
}

inline bool is_even_digits(const wstring& s) { return !s.empty() && ((s.back() - L'0') % 2 == 0); }

// 把 "4000" / "4000k" / "5M" / "2.5m" 解析成 kbps。纯数字按 kbps 理解。无效返回 false。
inline bool parse_kbps(const wstring& text, long long& out) {
    wstring t = trim(text);
    size_t i = 0, n = t.size();
    while (i < n && is_digit(t[i])) ++i;
    if (i == 0) return false;
    if (i < n && t[i] == L'.') {
        size_t j = i + 1;
        while (j < n && is_digit(t[j])) ++j;
        if (j > i + 1) i = j;  // 小数点后必须有数字才算数的一部分
    }
    string num;
    for (size_t k = 0; k < i; ++k) num.push_back(static_cast<char>(t[k]));
    while (i < n && is_space(t[i])) ++i;
    wchar_t suffix = 0;
    if (i < n && (t[i] == L'k' || t[i] == L'K' || t[i] == L'm' || t[i] == L'M')) suffix = t[i++];
    if (i != n) return false;
    double v = std::strtod(num.c_str(), nullptr);
    if (suffix == L'm' || suffix == L'M') v *= 1000;
    if (v > 1e12) v = 1e12;
    long long k = static_cast<long long>(std::nearbyint(v));
    if (k <= 0) return false;
    out = k;
    return true;
}

// 帧率:30 / 29.97 / 30000/1001
inline bool is_valid_fps(const wstring& s) {
    size_t i = 0, n = s.size();
    size_t d0 = i;
    while (i < n && is_digit(s[i])) ++i;
    if (i == d0) return false;
    if (i < n && s[i] == L'.') {
        size_t j = i + 1;
        while (j < n && is_digit(s[j])) ++j;
        if (j == i + 1) return false;
        i = j;
    }
    if (i < n && s[i] == L'/') {
        size_t j = i + 1;
        while (j < n && is_digit(s[j])) ++j;
        if (j == i + 1) return false;
        i = j;
    }
    return i == n;
}

// ---------------------------------------------------------------------------
// 命令行的拼接 / 拆分(与界面里“完整命令行”文本框配合)
// ---------------------------------------------------------------------------
inline bool has_space(const wstring& s) {
    for (wchar_t c : s)
        if (is_space(c)) return true;
    return false;
}

// 显示用:含空白或为空才加引号,引号里的 " 写成 \"
inline wstring quote_arg(const wstring& arg) {
    if (arg.empty() || has_space(arg)) {
        wstring r = L"\"";
        for (wchar_t c : arg) {
            if (c == L'"')
                r += L"\\\"";
            else
                r += c;
        }
        r += L'"';
        return r;
    }
    return arg;
}

inline wstring join_cmd(const vector<wstring>& cmd) {
    wstring r;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i) r += L' ';
        r += quote_arg(cmd[i]);
    }
    return r;
}

// 把文本框内容拆成参数:"..." 为一个参数(内部 \" 还原成 "),其余按空白分割
inline vector<wstring> split_cmd(const wstring& s) {
    vector<wstring> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (is_space(s[i])) {
            ++i;
            continue;
        }
        if (s[i] == L'"') {
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                wchar_t c = s[j];
                if (c == L'\\') {
                    if (j + 1 < n && s[j + 1] != L'\n') {
                        j += 2;
                        continue;
                    }
                    break;
                }
                if (c == L'"') {
                    closed = true;
                    break;
                }
                ++j;
            }
            if (closed) {
                wstring raw = s.substr(i + 1, j - i - 1), val;
                for (size_t k = 0; k < raw.size(); ++k) {
                    if (raw[k] == L'\\' && k + 1 < raw.size() && raw[k + 1] == L'"') {
                        val += L'"';
                        ++k;
                    } else {
                        val += raw[k];
                    }
                }
                out.push_back(val);
                i = j + 1;
                continue;
            }
        }
        size_t j = i;
        while (j < n && !is_space(s[j])) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// 交给 CreateProcessW 的命令行(Windows 标准的参数转义规则)
inline wstring win_quote(const wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == wstring::npos) return arg;
    wstring r = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t bs = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++bs;
        }
        if (it == arg.end()) {
            r.append(bs * 2, L'\\');
            break;
        } else if (*it == L'"') {
            r.append(bs * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(bs, L'\\');
            r.push_back(*it);
        }
    }
    r.push_back(L'"');
    return r;
}

inline wstring win_cmdline(const vector<wstring>& cmd) {
    wstring r;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i) r += L' ';
        r += win_quote(cmd[i]);
    }
    return r;
}

// ---------------------------------------------------------------------------
// 路径小工具(同时认 / 和 \)
// ---------------------------------------------------------------------------
inline wstring path_dirname(const wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    if (i == wstring::npos) return L"";
    if (i == 0) return p.substr(0, 1);
    return p.substr(0, i);
}
inline wstring path_basename(const wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}
inline wstring path_stem(const wstring& p) {
    wstring b = path_basename(p);
    size_t d = b.rfind(L'.');
    return (d == wstring::npos || d == 0) ? b : b.substr(0, d);
}
inline wstring path_ext(const wstring& p) {  // 含点,如 ".mp3";没有则为空
    wstring b = path_basename(p);
    size_t d = b.rfind(L'.');
    return (d == wstring::npos || d == 0) ? L"" : b.substr(d);
}
inline wstring path_join_like(const wstring& dir, const wstring& name, const wstring& like) {
    size_t i = like.find_last_of(L"\\/");
    wchar_t sep =
#ifdef _WIN32
        L'\\';
#else
        L'/';
#endif
    if (i != wstring::npos) sep = like[i];
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) return dir + name;
    return dir + sep + name;
}

// 图片+音频:输出为空时,按音频(没有则按图片)所在目录生成 <名字>.mp4;无法确定则返回空
inline wstring auto_output_image_audio(const wstring& audio, const wstring& image) {
    const wstring& src = !audio.empty() ? audio : image;
    if (src.empty()) return L"";
    wstring dir = path_dirname(src);
    if (dir.empty()) return L"";
    return path_join_like(dir, path_stem(src) + L".mp4", src);
}
// 视频+音频:<视频名>_with_audio.mp4,放在视频所在目录
inline wstring auto_output_video(const wstring& video) {
    if (video.empty()) return L"";
    wstring dir = path_dirname(video);
    if (dir.empty()) return L"";
    return path_join_like(dir, path_stem(video) + L"_with_audio.mp4", video);
}

// ---------------------------------------------------------------------------
// 标签页 1:图片 + 音频 -> MP4
// ---------------------------------------------------------------------------
struct ImageAudioOpts {
    wstring ffmpeg, image, audio, output;
    wstring width = L"1280", height = L"720";
    bool scale_pad = true;
    wstring vcodec = L"libx264", tune = L"stillimage";
    bool use_tune = true;
    wstring acodec = L"aac", pixfmt = L"yuv420p";
    bool shortest = true, overwrite = true;
    wstring extra;
};

inline wstring or_default(const wstring& v, const wchar_t* def) {
    wstring t = trim(v);
    return t.empty() ? wstring(def) : t;
}

inline vector<wstring> build_image_audio(const ImageAudioOpts& o) {
    vector<wstring> cmd;
    cmd.push_back(or_default(o.ffmpeg, L"ffmpeg"));
    if (o.overwrite) cmd.push_back(L"-y");

    cmd.insert(cmd.end(), {L"-loop", L"1", L"-i", or_default(o.image, PH_IMAGE)});
    cmd.insert(cmd.end(), {L"-i", or_default(o.audio, PH_AUDIO)});
    cmd.insert(cmd.end(), {L"-c:v", or_default(o.vcodec, L"libx264")});

    if (o.use_tune && !trim(o.tune).empty()) cmd.insert(cmd.end(), {L"-tune", trim(o.tune)});

    cmd.insert(cmd.end(), {L"-c:a", or_default(o.acodec, L"aac")});
    if (o.shortest) cmd.push_back(L"-shortest");
    cmd.insert(cmd.end(), {L"-pix_fmt", or_default(o.pixfmt, L"yuv420p")});

    wstring w = or_default(o.width, L"1280"), h = or_default(o.height, L"720");
    if (o.scale_pad) {
        wstring vf = L"scale=" + w + L":" + h + L":force_original_aspect_ratio=decrease,pad=" + w + L":" + h +
                     L":(ow-iw)/2:(oh-ih)/2";
        cmd.insert(cmd.end(), {L"-vf", vf});
    }

    wstring extra = trim(o.extra);
    if (!extra.empty()) {
        for (auto& a : split_cmd(extra)) cmd.push_back(a);
    }
    cmd.push_back(or_default(o.output, PH_OUTPUT));
    return cmd;
}

// ---------------------------------------------------------------------------
// 标签页 2:无声视频 + 音频(mp3 / aac ...) -> 带声音的 MP4
// ---------------------------------------------------------------------------
struct VideoAudioOpts {
    wstring ffmpeg, video, audio, output;

    // 视频
    bool reencode = true;
    bool res_keep = true;  // “保持原分辨率”
    wstring width = L"1920", height = L"1080";
    bool pad = true;  // 保持宽高比并填充黑边
    wstring fps = FPS_KEEP;
    wstring vcodec = L"libx264", preset = L"medium";
    bool crf_mode = true;  // true: CRF;false: 目标码率
    wstring crf = L"23", vbr = L"4000", maxrate, pixfmt = L"yuv420p";

    // 音频
    wstring acodec = L"aac", abitrate = L"192k", arate = KEEP_SAME, achannels = KEEP_SAME;

    // 合成
    DurMode mode = DurMode::Shortest;
    bool faststart = true, overwrite = true;
    wstring extra;
};

struct Built {
    vector<wstring> cmd;
    vector<wstring> notes;  // 参数冲突 / 被忽略的参数等提示
};

// 被循环(-stream_loop)的那一路如果是直接复制,-shortest 不会生效,ffmpeg 会一直写下去,
// 所以循环模式下被循环的那一路必须重新编码。
inline bool effective_reencode(const VideoAudioOpts& o) { return o.reencode || o.mode == DurMode::AudioLoopVideo; }

inline wstring effective_acodec(const VideoAudioOpts& o) {
    wstring a = or_default(o.acodec, L"aac");
    if (a == L"copy" && (o.mode == DurMode::VideoPad || o.mode == DurMode::VideoLoopAudio)) return L"aac";
    return a;
}

inline void video_args(const VideoAudioOpts& o, Built& b) {
    auto& args = b.cmd;
    auto note = [&](const wstring& s) { b.notes.push_back(s); };

    wstring vcodec = or_default(o.vcodec, L"libx264");
    bool is_x26x = (vcodec == L"libx264" || vcodec == L"libx265");
    bool is_nvenc = vcodec.size() >= 6 && vcodec.compare(vcodec.size() - 6, 6, L"_nvenc") == 0;
    args.insert(args.end(), {L"-c:v", vcodec});

    wstring preset = trim(o.preset);
    if (is_x26x && !preset.empty()) args.insert(args.end(), {L"-preset", preset});

    // 画质 / 码率
    if (o.crf_mode) {
        wstring crf = trim(o.crf);
        bool ok = (crf.size() == 1 || crf.size() == 2);
        for (wchar_t c : crf)
            if (!is_digit(c)) ok = false;
        if (!ok) {
            if (!crf.empty()) note(L"CRF 应为 0~51 的整数,已改用 23");
            crf = L"23";
        }
        if (is_x26x) {
            args.insert(args.end(), {L"-crf", crf});
        } else if (is_nvenc) {
            args.insert(args.end(), {L"-rc", L"vbr", L"-cq", crf, L"-b:v", L"0"});
        } else {
            note(L"编码器 " + vcodec + L" 未适配 CRF,未添加画质参数,请改用目标码率或在额外参数中指定");
        }
    } else {
        long long kbps = 0;
        if (parse_kbps(o.vbr, kbps))
            args.insert(args.end(), {L"-b:v", std::to_wstring(kbps) + L"k"});
        else
            note(L"目标码率无效,已忽略");
    }

    wstring maxrate_text = trim(o.maxrate);
    if (!maxrate_text.empty()) {
        long long maxrate = 0;
        if (parse_kbps(maxrate_text, maxrate))
            args.insert(args.end(), {L"-maxrate", std::to_wstring(maxrate) + L"k", L"-bufsize",
                                     std::to_wstring(maxrate * 2) + L"k"});
        else
            note(L"最大码率无效,已忽略");
    }

    wstring pixfmt = trim(o.pixfmt);
    if (!pixfmt.empty()) args.insert(args.end(), {L"-pix_fmt", pixfmt});

    // 分辨率
    if (!o.res_keep) {
        wstring w = trim(o.width), h = trim(o.height);
        if (is_posint(w) && is_posint(h)) {
            if (!is_even_digits(w) || !is_even_digits(h)) note(L"宽、高建议取偶数(yuv420p 要求),否则 ffmpeg 可能报错");
            wstring vf;
            if (o.pad)
                vf = L"scale=" + w + L":" + h + L":force_original_aspect_ratio=decrease,pad=" + w + L":" + h +
                     L":(ow-iw)/2:(oh-ih)/2,setsar=1";
            else
                vf = L"scale=" + w + L":" + h + L",setsar=1";
            args.insert(args.end(), {L"-vf", vf});
        } else {
            note(L"分辨率的宽、高必须是正整数,已保持原分辨率");
        }
    }

    // 帧率
    wstring fps = trim(o.fps);
    if (!fps.empty() && fps != FPS_KEEP) {
        if (is_valid_fps(fps))
            args.insert(args.end(), {L"-r", fps});
        else
            note(L"帧率格式无效(可写 30、29.97 或 30000/1001),已保持原帧率");
    }

    if (vcodec == L"libx265" || vcodec == L"hevc_nvenc") args.insert(args.end(), {L"-tag:v", L"hvc1"});  // 让 Apple 设备识别 HEVC
}

inline void audio_args(const VideoAudioOpts& o, Built& b) {
    auto& args = b.cmd;
    auto note = [&](const wstring& s) { b.notes.push_back(s); };

    wstring acodec = effective_acodec(o);
    if (acodec != or_default(o.acodec, L"aac")) {
        if (o.mode == DurMode::VideoPad)
            note(L"“音频不足补静音”需要重新编码音频,已改用 aac");
        else
            note(L"循环音频需要重新编码音频,已改用 aac(直接复制时 ffmpeg 无法按视频时长结束)");
    }

    if (acodec == L"copy") {
        args.insert(args.end(), {L"-c:a", L"copy"});
        wstring ext = lower(path_ext(o.audio));
        if (!ext.empty() && ext != L".mp3" && ext != L".aac" && ext != L".m4a" && ext != L".mp4")
            note(ext + L" 音频直接复制进 MP4 可能不被支持,建议音频编码器选 aac");
        return;
    }

    args.insert(args.end(), {L"-c:a", acodec});

    wstring abitrate = trim(o.abitrate);
    if (!abitrate.empty()) {
        long long kbps = 0;
        if (parse_kbps(abitrate, kbps))
            args.insert(args.end(), {L"-b:a", std::to_wstring(kbps) + L"k"});
        else
            note(L"音频码率无效,已忽略");
    }

    wstring arate = trim(o.arate);
    if (!arate.empty() && arate != KEEP_SAME) {
        if (is_posint(arate))
            args.insert(args.end(), {L"-ar", arate});
        else
            note(L"采样率必须是正整数,已保持原样");
    }

    wstring channels = trim(o.achannels);
    if (!channels.empty() && channels != KEEP_SAME) {
        size_t sp = channels.find(L' ');
        args.insert(args.end(), {L"-ac", sp == wstring::npos ? channels : channels.substr(0, sp)});
    }
}

inline Built build_video_audio(const VideoAudioOpts& o) {
    Built b;
    auto& cmd = b.cmd;
    cmd.push_back(or_default(o.ffmpeg, L"ffmpeg"));
    if (o.overwrite) cmd.push_back(L"-y");

    // 循环输入要写在对应的 -i 之前
    if (o.mode == DurMode::AudioLoopVideo) cmd.insert(cmd.end(), {L"-stream_loop", L"-1"});
    cmd.insert(cmd.end(), {L"-i", or_default(o.video, PH_VIDEO)});
    if (o.mode == DurMode::VideoLoopAudio) cmd.insert(cmd.end(), {L"-stream_loop", L"-1"});
    cmd.insert(cmd.end(), {L"-i", or_default(o.audio, PH_AUDIO)});

    // 明确只取视频文件的画面、音频文件的声音
    cmd.insert(cmd.end(), {L"-map", L"0:v:0", L"-map", L"1:a:0"});

    if (effective_reencode(o)) {
        if (!o.reencode)
            b.notes.push_back(L"循环视频需要重新编码视频,已自动启用重新编码(直接复制时 ffmpeg 无法按音频时长结束)");
        video_args(o, b);
    } else {
        cmd.insert(cmd.end(), {L"-c:v", L"copy"});
    }

    audio_args(o, b);
    if (o.mode == DurMode::VideoPad) cmd.insert(cmd.end(), {L"-af", L"apad"});

    cmd.push_back(L"-shortest");
    if (o.faststart) cmd.insert(cmd.end(), {L"-movflags", L"+faststart"});

    wstring extra = trim(o.extra);
    if (!extra.empty()) {
        for (auto& a : split_cmd(extra)) cmd.push_back(a);
    }
    cmd.push_back(or_default(o.output, PH_OUTPUT));
    return b;
}

// 界面上显示的提示文字:没有提示则为空
inline wstring notes_text(const vector<wstring>& notes) {
    if (notes.empty()) return L"";
    wstring r = L"提示:";
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i) r += L";";
        r += notes[i];
    }
    return r;
}

// ---------------------------------------------------------------------------
// ffprobe 输出解析
// 调用方式:ffprobe -v error -show_entries
//   stream=codec_type,codec_name,width,height,avg_frame_rate,r_frame_rate,bit_rate,sample_rate,channels
//   :format=duration,bit_rate  <文件>
// 输出形如 [STREAM] key=value ... [/STREAM] [FORMAT] key=value ... [/FORMAT]
// ---------------------------------------------------------------------------
inline const char* probe_show_entries() {
    return "stream=codec_type,codec_name,width,height,avg_frame_rate,r_frame_rate,bit_rate,sample_rate,channels"
           ":format=duration,bit_rate";
}

struct ProbeBlock {
    std::map<string, string> kv;
    string get(const char* k) const {  // 缺失或 N/A 都当作没有
        auto it = kv.find(k);
        if (it == kv.end() || it->second == "N/A") return "";
        return it->second;
    }
};
struct ProbeInfo {
    vector<ProbeBlock> streams;
    ProbeBlock format;
};

inline ProbeInfo parse_probe(const string& text) {
    ProbeInfo info;
    ProbeBlock cur;
    enum { None, Stream, Format } state = None;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        string line = text.substr(pos, nl == string::npos ? string::npos : nl - pos);
        pos = (nl == string::npos) ? text.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "[STREAM]") {
            state = Stream;
            cur = ProbeBlock();
        } else if (line == "[FORMAT]") {
            state = Format;
            cur = ProbeBlock();
        } else if (line == "[/STREAM]") {
            if (state == Stream) info.streams.push_back(cur);
            state = None;
        } else if (line == "[/FORMAT]") {
            if (state == Format) info.format = cur;
            state = None;
        } else if (state != None) {
            size_t eq = line.find('=');
            if (eq != string::npos) cur.kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return info;
}

inline wstring widen_ascii(const string& s) {
    wstring w;
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}
inline wstring fmt_double(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return widen_ascii(buf);
}
inline double to_float(const string& s) {
    if (s.empty()) return 0.0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    return (end && *end == '\0') ? v : 0.0;
}
inline double parse_fps(const string& rate) {  // "30000/1001" -> 29.97
    size_t sl = rate.find('/');
    if (sl == string::npos) return to_float(rate);
    double num = to_float(rate.substr(0, sl)), den = to_float(rate.substr(sl + 1));
    return den != 0 ? num / den : 0.0;
}

// 整理成一行说明。ok=false 表示 ffprobe 没能读出信息。video=true 为视频文件,否则为音频文件。
inline wstring describe_media(bool ok, const string& probe_text, bool video) {
    if (!ok) return L"(无法读取媒体信息:文件可能已损坏或格式不受支持)";
    ProbeInfo info = parse_probe(probe_text);
    double duration = to_float(info.format.get("duration"));
    auto join = [](const vector<wstring>& parts) {
        wstring r;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) r += L" · ";
            r += parts[i];
        }
        return r;
    };

    if (video) {
        const ProbeBlock* v = nullptr;
        bool has_audio = false;
        for (auto& s : info.streams) {
            if (!v && s.get("codec_type") == "video") v = &s;
            if (s.get("codec_type") == "audio") has_audio = true;
        }
        if (!v) return L"(文件中没有视频流)";
        vector<wstring> parts;
        if (!v->get("width").empty() && !v->get("height").empty())
            parts.push_back(widen_ascii(v->get("width")) + L"x" + widen_ascii(v->get("height")));
        string rate = !v->get("avg_frame_rate").empty() ? v->get("avg_frame_rate") : v->get("r_frame_rate");
        double fps = parse_fps(rate);
        if (fps != 0) {
            string s;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.2f", fps);
            s = buf;
            while (!s.empty() && s.back() == '0') s.pop_back();
            while (!s.empty() && s.back() == '.') s.pop_back();
            parts.push_back(widen_ascii(s) + L" fps");
        }
        if (!v->get("codec_name").empty()) parts.push_back(widen_ascii(v->get("codec_name")));
        double br = to_float(v->get("bit_rate"));
        if (br == 0) br = to_float(info.format.get("bit_rate"));
        if (br != 0) parts.push_back(fmt_double("%.0f", br / 1000) + L" kb/s");
        if (duration != 0) parts.push_back(fmt_double("%.2f", duration) + L" 秒");
        wstring text = L"原视频: " + join(parts);
        if (has_audio) text += L"  (该文件含音轨,合成时会被替换)";
        return text;
    }

    const ProbeBlock* a = nullptr;
    for (auto& s : info.streams)
        if (s.get("codec_type") == "audio") {
            a = &s;
            break;
        }
    if (!a) return L"(文件中没有音频流)";
    vector<wstring> parts;
    if (!a->get("codec_name").empty()) parts.push_back(widen_ascii(a->get("codec_name")));
    if (!a->get("sample_rate").empty()) parts.push_back(widen_ascii(a->get("sample_rate")) + L" Hz");
    string ch = a->get("channels");
    if (!ch.empty() && ch != "0") {
        if (ch == "1")
            parts.push_back(L"单声道");
        else if (ch == "2")
            parts.push_back(L"立体声");
        else
            parts.push_back(widen_ascii(ch) + L" 声道");
    }
    double br = to_float(a->get("bit_rate"));
    if (br == 0) br = to_float(info.format.get("bit_rate"));
    if (br != 0) parts.push_back(fmt_double("%.0f", br / 1000) + L" kb/s");
    if (duration != 0) parts.push_back(fmt_double("%.2f", duration) + L" 秒");
    return L"原音频: " + join(parts);
}

// ---------------------------------------------------------------------------
// UTF-8 <-> wstring(可移植,Windows 上 wchar_t 为 UTF-16,Linux 上为 UTF-32)
// ---------------------------------------------------------------------------
inline string to_utf8(const wstring& w) {
    string out;
    for (size_t i = 0; i < w.size(); ++i) {
        unsigned long cp = static_cast<unsigned long>(w[i]);
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            unsigned long lo = static_cast<unsigned long>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline wstring from_utf8(const string& s) {
    wstring out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned long cp;
        int extra;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0F;
            extra = 2;
        } else {
            cp = c & 0x07;
            extra = 3;
        }
        ++i;
        while (extra-- > 0 && i < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

}  // namespace core
