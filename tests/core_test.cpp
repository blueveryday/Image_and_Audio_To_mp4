// core_test.cpp —— core.hpp 的测试(Linux / macOS 下运行,需要 PATH 里有 ffmpeg 和 ffprobe)。
//   单元测试:命令拆分与转义、码率/帧率解析、路径、命令生成、ffprobe 输出解析
//   集成测试:用生成的命令真正调用 ffmpeg,再用 ffprobe 检查输出文件
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/core.hpp"

using namespace core;

static int g_pass = 0;
static std::vector<std::string> g_fail;

static std::string U(const wstring& w) { return to_utf8(w); }

#define CHECK(name, cond)                                          \
    do {                                                           \
        if (cond) {                                                \
            ++g_pass;                                              \
        } else {                                                   \
            g_fail.push_back(name);                                \
            std::printf("  FAIL %s  (line %d)\n", name, __LINE__); \
        }                                                          \
    } while (0)

#define CHECK_EQ(name, a, b)                                                                       \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_pass;                                                                              \
        } else {                                                                                   \
            g_fail.push_back(name);                                                                \
            std::printf("  FAIL %s  (line %d)\n    got:      %s\n    expected: %s\n", name, __LINE__, \
                        U(_a).c_str(), U(_b).c_str());                                             \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
static std::string sh_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'')
            r += "'\\''";
        else
            r += c;
    }
    return r + "'";
}

static std::string capture(const std::string& cmdline, int* rc = nullptr) {
    std::string out;
    FILE* p = popen(cmdline.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int status = pclose(p);
    if (rc) *rc = status;
    return out;
}

// 运行 core 生成的命令(带超时),返回退出码;124 表示超时
static int run_cmd(const vector<wstring>& cmd) {
    std::string line = "timeout 45";
    for (auto& a : cmd) line += " " + sh_quote(U(a));
    line += " </dev/null >/dev/null 2>&1";
    int st = std::system(line.c_str());
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

struct Probed {
    bool ok = false;
    ProbeInfo info;
    double duration = 0;
    const ProbeBlock* vstream() const {
        for (auto& s : info.streams)
            if (s.get("codec_type") == "video") return &s;
        return nullptr;
    }
    vector<const ProbeBlock*> astreams() const {
        vector<const ProbeBlock*> r;
        for (auto& s : info.streams)
            if (s.get("codec_type") == "audio") r.push_back(&s);
        return r;
    }
};

static Probed probe(const wstring& path) {
    Probed p;
    int rc = 0;
    std::string out = capture(
        "ffprobe -v error -show_entries "
        "stream=codec_type,codec_name,codec_tag_string,width,height,avg_frame_rate,sample_rate,channels"
        ":format=duration " +
            sh_quote(U(path)) + " 2>/dev/null",
        &rc);
    p.ok = (rc == 0);
    p.info = parse_probe(out);
    p.duration = to_float(p.info.format.get("duration"));
    return p;
}

static double fps_of(const ProbeBlock& s) { return parse_fps(s.get("avg_frame_rate")); }
static int to_i(const std::string& s) { return std::atoi(s.c_str()); }
static bool contains(const wstring& hay, const wstring& needle) { return hay.find(needle) != wstring::npos; }
static bool has_arg(const vector<wstring>& cmd, const wstring& a) {
    for (auto& c : cmd)
        if (c == a) return true;
    return false;
}
static wstring arg_after(const vector<wstring>& cmd, const wstring& a) {
    for (size_t i = 0; i + 1 < cmd.size(); ++i)
        if (cmd[i] == a) return cmd[i + 1];
    return L"";
}
static size_t index_of(const vector<wstring>& cmd, const wstring& a) {
    for (size_t i = 0; i < cmd.size(); ++i)
        if (cmd[i] == a) return i;
    return static_cast<size_t>(-1);
}

// ---------------------------------------------------------------------------
// 单元测试
// ---------------------------------------------------------------------------
static void test_strings() {
    std::puts("== 字符串 / 命令行");
    CHECK_EQ("quote 普通参数不加引号", quote_arg(L"abc"), wstring(L"abc"));
    CHECK_EQ("quote 含空格加引号", quote_arg(L"a b"), wstring(L"\"a b\""));
    CHECK_EQ("quote 空参数", quote_arg(L""), wstring(L"\"\""));
    CHECK_EQ("quote 内部引号转义", quote_arg(L"a \"b\""), wstring(L"\"a \\\"b\\\"\""));

    auto t = split_cmd(L"ffmpeg -i \"C:\\my dir\\a b.mp3\" -vf scale=1:2  out.mp4 ");
    CHECK("split 个数", t.size() == 6);
    if (t.size() == 6) {
        CHECK_EQ("split 带空格的路径", t[2], wstring(L"C:\\my dir\\a b.mp3"));
        CHECK_EQ("split 末项", t[5], wstring(L"out.mp4"));
    }
    auto e = split_cmd(L"a \"\" b");
    CHECK("split 空引号参数保留为空串", e.size() == 3 && e[1].empty());
    auto q = split_cmd(L"\"say \\\"hi\\\"\"");
    CHECK("split 转义引号", q.size() == 1 && q[0] == L"say \"hi\"");
    auto u = split_cmd(L"\"unclosed arg");
    CHECK("split 未闭合引号按普通参数", u.size() == 2 && u[0] == L"\"unclosed");
    auto adj = split_cmd(L"\"a\"b");
    CHECK("split 引号后紧跟文字", adj.size() == 2 && adj[0] == L"a" && adj[1] == L"b");
    CHECK("split 空串", split_cmd(L"   ").empty());

    // 拼接再拆分应还原
    vector<wstring> orig = {L"ffmpeg", L"-i", L"C:\\a b\\x.mp4", L"", L"-vf", L"scale=1:2,pad=1:2:(ow-iw)/2:(oh-ih)/2",
                            L"say \"hi\"", L"最终 文件.mp4"};
    auto back = split_cmd(join_cmd(orig));
    CHECK("join/split 往返", back == orig);

    CHECK_EQ("win_quote 普通", win_quote(L"abc"), wstring(L"abc"));
    CHECK_EQ("win_quote 空格", win_quote(L"a b"), wstring(L"\"a b\""));
    CHECK_EQ("win_quote 空串", win_quote(L""), wstring(L"\"\""));
    CHECK_EQ("win_quote 引号", win_quote(L"a\"b"), wstring(L"\"a\\\"b\""));
    CHECK_EQ("win_quote 结尾反斜杠", win_quote(L"C:\\a b\\"), wstring(L"\"C:\\a b\\\\\""));
    CHECK_EQ("win_quote 中间反斜杠不变", win_quote(L"C:\\a\\b c"), wstring(L"\"C:\\a\\b c\""));

    CHECK_EQ("trim", trim(L"  x y \t\r\n"), wstring(L"x y"));
    CHECK("utf8 往返", from_utf8(to_utf8(L"视频 + 音频 ✓ abc")) == L"视频 + 音频 ✓ abc");
}

static void test_numbers() {
    std::puts("== 码率 / 帧率 / 整数解析");
    long long v = 0;
    CHECK("kbps 4000", parse_kbps(L"4000", v) && v == 4000);
    CHECK("kbps 4000k", parse_kbps(L"4000k", v) && v == 4000);
    CHECK("kbps 5M", parse_kbps(L"5M", v) && v == 5000);
    CHECK("kbps 2.5m", parse_kbps(L"2.5m", v) && v == 2500);
    CHECK("kbps 带空白", parse_kbps(L"  300 k ", v) && v == 300);
    CHECK("kbps 小数 1.5 取整", parse_kbps(L"1.5", v) && v == 2);
    CHECK("kbps 非法 x", !parse_kbps(L"x", v));
    CHECK("kbps 非法 空", !parse_kbps(L"", v));
    CHECK("kbps 非法 0", !parse_kbps(L"0", v));
    CHECK("kbps 非法 5.", !parse_kbps(L"5.", v));
    CHECK("kbps 非法 5kk", !parse_kbps(L"5kk", v));
    CHECK("kbps 非法 -5", !parse_kbps(L"-5", v));

    CHECK("fps 30", is_valid_fps(L"30"));
    CHECK("fps 29.97", is_valid_fps(L"29.97"));
    CHECK("fps 30000/1001", is_valid_fps(L"30000/1001"));
    CHECK("fps 非法 fast", !is_valid_fps(L"fast"));
    CHECK("fps 非法 30.", !is_valid_fps(L"30."));
    CHECK("fps 非法 /25", !is_valid_fps(L"/25"));
    CHECK("fps 非法 空", !is_valid_fps(L""));

    CHECK("posint 1280", is_posint(L"1280"));
    CHECK("posint 0", !is_posint(L"0"));
    CHECK("posint 000", !is_posint(L"000"));
    CHECK("posint abc", !is_posint(L"abc"));
    CHECK("posint -1", !is_posint(L"-1"));
    CHECK("posint 空", !is_posint(L""));
}

static void test_paths() {
    std::puts("== 路径 / 默认输出名");
    CHECK_EQ("dirname win", path_dirname(L"C:\\a\\b\\c.mp3"), wstring(L"C:\\a\\b"));
    CHECK_EQ("dirname unix", path_dirname(L"/a/b/c.mp3"), wstring(L"/a/b"));
    CHECK_EQ("dirname 无目录", path_dirname(L"c.mp3"), wstring(L""));
    CHECK_EQ("stem", path_stem(L"C:\\a\\b.c.mp3"), wstring(L"b.c"));
    CHECK_EQ("stem 隐藏文件", path_stem(L"/a/.bashrc"), wstring(L".bashrc"));
    CHECK_EQ("ext", path_ext(L"C:\\a\\b.MP3"), wstring(L".MP3"));
    CHECK_EQ("ext 无", path_ext(L"C:\\a.b\\c"), wstring(L""));

    CHECK_EQ("自动输出 视频(win)", auto_output_video(L"C:\\v\\clip.mp4"), wstring(L"C:\\v\\clip_with_audio.mp4"));
    CHECK_EQ("自动输出 视频(unix)", auto_output_video(L"/v/clip.mp4"), wstring(L"/v/clip_with_audio.mp4"));
    CHECK_EQ("自动输出 视频 无目录", auto_output_video(L"clip.mp4"), wstring(L""));
    CHECK_EQ("自动输出 视频 空", auto_output_video(L""), wstring(L""));
    CHECK_EQ("自动输出 图片+音频 以音频为准", auto_output_image_audio(L"D:\\m\\song.mp3", L"E:\\p\\cover.png"),
             wstring(L"D:\\m\\song.mp4"));
    CHECK_EQ("自动输出 只有图片", auto_output_image_audio(L"", L"E:\\p\\cover.png"), wstring(L"E:\\p\\cover.mp4"));
    CHECK_EQ("自动输出 都没有", auto_output_image_audio(L"", L""), wstring(L""));
}

static void test_image_audio_cmd() {
    std::puts("== 标签页 1 命令生成");
    ImageAudioOpts o;
    o.ffmpeg = L"ffmpeg.exe";
    o.image = L"C:\\p\\a b.png";
    o.audio = L"C:\\p\\a.mp3";
    o.output = L"C:\\p\\out.mp4";
    CHECK_EQ("默认命令(与原版逻辑一致)", join_cmd(build_image_audio(o)),
             wstring(L"ffmpeg.exe -y -loop 1 -i \"C:\\p\\a b.png\" -i C:\\p\\a.mp3 -c:v libx264 -tune stillimage "
                     L"-c:a aac -shortest -pix_fmt yuv420p -vf "
                     L"scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2 "
                     L"C:\\p\\out.mp4"));

    ImageAudioOpts e;  // 全空 => 占位符与默认值
    CHECK_EQ("空白时用占位符", join_cmd(build_image_audio(e)),
             wstring(L"ffmpeg -y -loop 1 -i <图片文件> -i <音频文件> -c:v libx264 -tune stillimage -c:a aac "
                     L"-shortest -pix_fmt yuv420p -vf "
                     L"scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2 "
                     L"<输出文件>"));

    ImageAudioOpts f = o;
    f.overwrite = false;
    f.use_tune = false;
    f.shortest = false;
    f.scale_pad = false;
    f.vcodec = L"libx265";
    f.acodec = L"libmp3lame";
    f.pixfmt = L"yuv444p";
    f.extra = L"-r 30 -metadata \"title=my video\"";
    auto c = build_image_audio(f);
    CHECK("取消勾选后无 -y/-tune/-shortest/-vf", !has_arg(c, L"-y") && !has_arg(c, L"-tune") &&
                                                      !has_arg(c, L"-shortest") && !has_arg(c, L"-vf"));
    CHECK_EQ("编码器", arg_after(c, L"-c:v"), wstring(L"libx265"));
    CHECK_EQ("额外参数被拆开(含引号)", arg_after(c, L"-metadata"), wstring(L"title=my video"));
    CHECK("输出在最后", c.back() == L"C:\\p\\out.mp4" && arg_after(c, L"-r") == L"30");
}

static void test_video_audio_cmd() {
    std::puts("== 标签页 2 命令生成与提示");
    VideoAudioOpts o;
    o.video = L"v.mp4";
    o.audio = L"a.mp3";
    o.output = L"o.mp4";

    // 默认值
    Built b = build_video_audio(o);
    CHECK_EQ("默认命令", join_cmd(b.cmd),
             wstring(L"ffmpeg -y -i v.mp4 -i a.mp3 -map 0:v:0 -map 1:a:0 -c:v libx264 -preset medium -crf 23 "
                     L"-pix_fmt yuv420p -c:a aac -b:a 192k -shortest -movflags +faststart o.mp4"));
    CHECK("默认无提示", b.notes.empty() && notes_text(b.notes).empty());

    // 720p + 24fps + 目标码率 + 最大码率 + 单声道 44.1k
    VideoAudioOpts p = o;
    p.res_keep = false;
    p.width = L"1280";
    p.height = L"720";
    p.fps = L"24";
    p.crf_mode = false;
    p.vbr = L"2000";
    p.maxrate = L"2500";
    p.abitrate = L"128k";
    p.arate = L"44100";
    p.achannels = L"1 (单声道)";
    b = build_video_audio(p);
    CHECK_EQ("720p/ABR 命令", join_cmd(b.cmd),
             wstring(L"ffmpeg -y -i v.mp4 -i a.mp3 -map 0:v:0 -map 1:a:0 -c:v libx264 -preset medium -b:v 2000k "
                     L"-maxrate 2500k -bufsize 5000k -pix_fmt yuv420p -vf "
                     L"scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2,setsar=1 "
                     L"-r 24 -c:a aac -b:a 128k -ar 44100 -ac 1 -shortest -movflags +faststart o.mp4"));
    CHECK("ABR 无 -crf", !has_arg(b.cmd, L"-crf"));

    // 不填黑边
    p.pad = false;
    b = build_video_audio(p);
    CHECK_EQ("拉伸不含 pad", arg_after(b.cmd, L"-vf"), wstring(L"scale=1280:720,setsar=1"));

    // 全部 copy
    VideoAudioOpts c = o;
    c.reencode = false;
    c.acodec = L"copy";
    b = build_video_audio(c);
    CHECK("copy: 无编码参数", !has_arg(b.cmd, L"-vf") && !has_arg(b.cmd, L"-crf") && !has_arg(b.cmd, L"-preset") &&
                                  !has_arg(b.cmd, L"-b:a"));
    CHECK_EQ("copy: -c:v", arg_after(b.cmd, L"-c:v"), wstring(L"copy"));
    CHECK_EQ("copy: -c:a", arg_after(b.cmd, L"-c:a"), wstring(L"copy"));
    CHECK("copy: 无提示(mp3 可直接复制)", b.notes.empty());
    c.audio = L"a.wav";
    b = build_video_audio(c);
    CHECK("copy wav 给出提示", b.notes.size() == 1 && contains(b.notes[0], L".wav"));

    // 补静音 + copy 冲突
    VideoAudioOpts pad = o;
    pad.mode = DurMode::VideoPad;
    pad.acodec = L"copy";
    b = build_video_audio(pad);
    CHECK_EQ("补静音: 音频改 aac", arg_after(b.cmd, L"-c:a"), wstring(L"aac"));
    CHECK_EQ("补静音: -af apad", arg_after(b.cmd, L"-af"), wstring(L"apad"));
    CHECK("补静音: 提示", b.notes.size() == 1 && contains(b.notes[0], L"补静音"));

    // 循环音频:-stream_loop 在音频 -i 之前,且 copy 被改 aac
    VideoAudioOpts la = o;
    la.mode = DurMode::VideoLoopAudio;
    la.acodec = L"copy";
    b = build_video_audio(la);
    size_t loop = index_of(b.cmd, L"-stream_loop");
    CHECK("循环音频: -stream_loop 紧邻音频 -i", loop != static_cast<size_t>(-1) && b.cmd[loop + 2] == L"-i" &&
                                                     b.cmd[loop + 3] == L"a.mp3");
    CHECK_EQ("循环音频: 音频改 aac", arg_after(b.cmd, L"-c:a"), wstring(L"aac"));
    CHECK("循环音频: 提示", b.notes.size() == 1 && contains(b.notes[0], L"循环音频"));
    la.reencode = false;  // 视频仍可直接复制
    b = build_video_audio(la);
    CHECK_EQ("循环音频: 视频仍可 copy", arg_after(b.cmd, L"-c:v"), wstring(L"copy"));

    // 循环视频:-stream_loop 在视频 -i 之前,且视频被强制重新编码
    VideoAudioOpts lv = o;
    lv.mode = DurMode::AudioLoopVideo;
    lv.reencode = false;
    b = build_video_audio(lv);
    loop = index_of(b.cmd, L"-stream_loop");
    CHECK("循环视频: -stream_loop 在视频 -i 之前", loop != static_cast<size_t>(-1) && b.cmd[loop + 2] == L"-i" &&
                                                         b.cmd[loop + 3] == L"v.mp4");
    CHECK_EQ("循环视频: 强制重新编码", arg_after(b.cmd, L"-c:v"), wstring(L"libx264"));
    CHECK("循环视频: 提示", !b.notes.empty() && contains(b.notes[0], L"循环视频"));
    CHECK("循环视频: effective_reencode", effective_reencode(lv));

    // HEVC / NVENC
    VideoAudioOpts h = o;
    h.vcodec = L"libx265";
    b = build_video_audio(h);
    CHECK("x265: hvc1 标签", arg_after(b.cmd, L"-tag:v") == L"hvc1" && arg_after(b.cmd, L"-crf") == L"23");
    h.vcodec = L"h264_nvenc";
    b = build_video_audio(h);
    CHECK("nvenc: -rc vbr -cq 23 -b:v 0 且无 -preset/-crf",
          arg_after(b.cmd, L"-rc") == L"vbr" && arg_after(b.cmd, L"-cq") == L"23" && arg_after(b.cmd, L"-b:v") == L"0" &&
              !has_arg(b.cmd, L"-preset") && !has_arg(b.cmd, L"-crf") && !has_arg(b.cmd, L"-tag:v"));
    h.vcodec = L"mpeg4";
    b = build_video_audio(h);
    CHECK("未适配编码器提示", b.notes.size() == 1 && contains(b.notes[0], L"未适配"));

    // 码率写法
    VideoAudioOpts r = o;
    r.crf_mode = false;
    r.vbr = L"5M";
    r.maxrate = L"6000";
    b = build_video_audio(r);
    CHECK("5M -> 5000k", arg_after(b.cmd, L"-b:v") == L"5000k");
    CHECK("maxrate 6000k / bufsize 12000k", arg_after(b.cmd, L"-maxrate") == L"6000k" && arg_after(b.cmd, L"-bufsize") == L"12000k");

    // 非法输入 -> 都有提示,命令里不含对应参数
    VideoAudioOpts bad = o;
    bad.res_keep = false;
    bad.width = L"abc";
    bad.fps = L"fast";
    bad.crf = L"99999";
    bad.maxrate = L"zz";
    bad.abitrate = L"lots";
    bad.arate = L"x";
    b = build_video_audio(bad);
    wstring nt = notes_text(b.notes);
    CHECK("非法: 分辨率提示", contains(nt, L"分辨率"));
    CHECK("非法: 帧率提示", contains(nt, L"帧率"));
    CHECK("非法: CRF 提示并回落到 23", contains(nt, L"CRF") && arg_after(b.cmd, L"-crf") == L"23");
    CHECK("非法: 最大码率提示", contains(nt, L"最大码率"));
    CHECK("非法: 音频码率提示", contains(nt, L"音频码率"));
    CHECK("非法: 采样率提示", contains(nt, L"采样率"));
    CHECK("非法: 命令里没有 -vf / -r / -maxrate / -ar", !has_arg(b.cmd, L"-vf") && !has_arg(b.cmd, L"-r") &&
                                                          !has_arg(b.cmd, L"-maxrate") && !has_arg(b.cmd, L"-ar"));
    CHECK("提示文本以“提示:”开头并用;连接", nt.rfind(L"提示:", 0) == 0 && contains(nt, L";"));
}

// ---------------------------------------------------------------------------
// ffprobe 输出解析
// ---------------------------------------------------------------------------
static void test_probe_parse() {
    std::puts("== ffprobe 输出解析");
    const std::string video_out =
        "[STREAM]\ncodec_type=video\ncodec_name=h264\nwidth=640\nheight=360\navg_frame_rate=25/1\nbit_rate=N/A\n"
        "[/STREAM]\n[STREAM]\ncodec_type=audio\ncodec_name=aac\nsample_rate=44100\nchannels=2\n[/STREAM]\n"
        "[FORMAT]\nduration=5.000000\nbit_rate=53000\n[/FORMAT]\n";
    wstring d = describe_media(true, video_out, true);
    CHECK_EQ("视频描述", d, wstring(L"原视频: 640x360 · 25 fps · h264 · 53 kb/s · 5.00 秒  (该文件含音轨,合成时会被替换)"));

    const std::string ntsc =
        "[STREAM]\ncodec_type=video\ncodec_name=hevc\nwidth=1920\nheight=1080\navg_frame_rate=30000/1001\n[/STREAM]\n"
        "[FORMAT]\nduration=10.5\nbit_rate=4000000\n[/FORMAT]\n";
    CHECK_EQ("NTSC 帧率 + 回落到容器码率", describe_media(true, ntsc, true),
             wstring(L"原视频: 1920x1080 · 29.97 fps · hevc · 4000 kb/s · 10.50 秒"));

    const std::string audio_out =
        "[STREAM]\ncodec_type=audio\ncodec_name=mp3\nsample_rate=44100\nchannels=1\nbit_rate=128000\n[/STREAM]\n"
        "[FORMAT]\nduration=4.048980\nbit_rate=128000\n[/FORMAT]\n";
    CHECK_EQ("音频描述", describe_media(true, audio_out, false),
             wstring(L"原音频: mp3 · 44100 Hz · 单声道 · 128 kb/s · 4.05 秒"));
    const std::string six = "[STREAM]\ncodec_type=audio\ncodec_name=ac3\nsample_rate=48000\nchannels=6\n[/STREAM]\n[FORMAT]\n[/FORMAT]\n";
    CHECK_EQ("6 声道", describe_media(true, six, false), wstring(L"原音频: ac3 · 48000 Hz · 6 声道"));

    CHECK_EQ("读取失败", describe_media(false, "", true),
             wstring(L"(无法读取媒体信息:文件可能已损坏或格式不受支持)"));
    CHECK_EQ("无视频流", describe_media(true, audio_out, true), wstring(L"(文件中没有视频流)"));
    CHECK_EQ("无音频流", describe_media(true, ntsc, false), wstring(L"(文件中没有音频流)"));
    CHECK("CRLF 换行也能解析", describe_media(true, "[STREAM]\r\ncodec_type=audio\r\ncodec_name=aac\r\n[/STREAM]\r\n", false) ==
                                   L"原音频: aac");
}

// ---------------------------------------------------------------------------
// 集成测试:真正调用 ffmpeg
// ---------------------------------------------------------------------------
static std::string g_dir;
static wstring P(const std::string& name) { return from_utf8(g_dir + "/" + name); }

static bool make_media() {
    auto sh = [](const std::string& c) { return std::system((c + " >/dev/null 2>&1").c_str()) == 0; };
    std::string d = sh_quote(g_dir);
    bool ok = true;
    ok &= sh("ffmpeg -y -f lavfi -i testsrc=size=1920x1080:rate=30 -t 6 -c:v libx264 -pix_fmt yuv420p -an " + d + "/silent.mp4");
    ok &= sh("ffmpeg -y -f lavfi -i sine=frequency=440:sample_rate=44100 -t 4 -c:a libmp3lame -b:a 128k " + d + "/a4.mp3");
    ok &= sh("ffmpeg -y -f lavfi -i sine=frequency=660:sample_rate=48000 -t 8 -c:a aac -b:a 128k " + d + "/a8.aac");
    ok &= sh("ffmpeg -y -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine=frequency=880 -t 5 -c:v libx264 "
             "-pix_fmt yuv420p -c:a aac " + d + "/with_audio.mp4");
    ok &= sh("ffmpeg -y -f lavfi -i color=c=blue:size=800x600 -frames:v 1 " + d + "/img.png");
    return ok;
}

// 用 opts 生成命令并运行;返回 ffprobe 结果。rc 为 ffmpeg 退出码。
static Probed run_video(const char* name, const std::string& out, VideoAudioOpts o, Built* built = nullptr,
                        int* rc_out = nullptr) {
    o.video = o.video.empty() ? P("silent.mp4") : o.video;
    o.audio = o.audio.empty() ? P("a4.mp3") : o.audio;
    o.output = P(out);
    Built b = build_video_audio(o);
    if (built) *built = b;
    std::remove((g_dir + "/" + out).c_str());
    int rc = run_cmd(b.cmd);
    if (rc_out) *rc_out = rc;
    std::string label = std::string(name) + ": ffmpeg 退出码 0";
    CHECK(label.c_str(), rc == 0);
    if (rc != 0) {
        std::printf("    命令: %s\n", U(join_cmd(b.cmd)).c_str());
        return Probed();
    }
    return probe(o.output);
}

static void test_integration() {
    std::puts("== 集成测试(真实 ffmpeg)");
    VideoAudioOpts base;
    base.ffmpeg = L"ffmpeg";

    {  // 1 默认参数
        Probed p = run_video("1 默认", "t1.mp4", base);
        auto v = p.vstream();
        auto a = p.astreams();
        CHECK("1 有视频流", v != nullptr);
        if (v) {
            CHECK("1 分辨率保持 1920x1080", to_i(v->get("width")) == 1920 && to_i(v->get("height")) == 1080);
            CHECK("1 帧率 30", std::fabs(fps_of(*v) - 30) < 0.1);
        }
        CHECK("1 恰好一条 aac 音轨", a.size() == 1 && a[0]->get("codec_name") == "aac");
        CHECK("1 时长≈4s(较短者)", std::fabs(p.duration - 4.05) < 0.3);
    }
    {  // 2 720p / 24fps / ABR / 单声道 / 44.1k,音频 8s 视频 6s
        VideoAudioOpts o = base;
        o.audio = P("a8.aac");
        o.res_keep = false;
        o.width = L"1280";
        o.height = L"720";
        o.fps = L"24";
        o.crf_mode = false;
        o.vbr = L"2000";
        o.maxrate = L"2500";
        o.abitrate = L"128k";
        o.arate = L"44100";
        o.achannels = L"1 (单声道)";
        Probed p = run_video("2 720p/24fps/ABR", "t2.mp4", o);
        auto v = p.vstream();
        auto a = p.astreams();
        if (v) {
            CHECK("2 分辨率 1280x720", to_i(v->get("width")) == 1280 && to_i(v->get("height")) == 720);
            CHECK("2 帧率 24", std::fabs(fps_of(*v) - 24) < 0.1);
        }
        CHECK("2 采样率 44100 / 单声道", a.size() == 1 && a[0]->get("sample_rate") == "44100" && to_i(a[0]->get("channels")) == 1);
        CHECK("2 时长≈6s(视频较短)", std::fabs(p.duration - 6.0) < 0.3);
    }
    {  // 3 全部 copy
        VideoAudioOpts o = base;
        o.reencode = false;
        o.acodec = L"copy";
        Probed p = run_video("3 全部 copy", "t3.mp4", o);
        auto v = p.vstream();
        auto a = p.astreams();
        CHECK("3 视频 h264 1920 未变", v && v->get("codec_name") == "h264" && to_i(v->get("width")) == 1920);
        CHECK("3 音频 mp3", a.size() == 1 && a[0]->get("codec_name") == "mp3");
    }
    {  // 4 补静音
        VideoAudioOpts o = base;
        o.mode = DurMode::VideoPad;
        Probed p = run_video("4 补静音", "t4.mp4", o);
        CHECK("4 时长≈6s", std::fabs(p.duration - 6.0) < 0.3);
        CHECK("4 有音轨", p.astreams().size() == 1);
    }
    {  // 5 循环音频;5b copy 被改成 aac 且不会卡死;5c 视频 copy
        VideoAudioOpts o = base;
        o.mode = DurMode::VideoLoopAudio;
        Probed p = run_video("5 循环音频", "t5.mp4", o);
        CHECK("5 时长≈6s", std::fabs(p.duration - 6.0) < 0.3);
        o.acodec = L"copy";
        p = run_video("5b 循环音频+copy音频", "t5b.mp4", o);
        CHECK("5b 时长≈6s", std::fabs(p.duration - 6.0) < 0.3);
        CHECK("5b 音频为 aac", p.astreams().size() == 1 && p.astreams()[0]->get("codec_name") == "aac");
        o.acodec = L"aac";
        o.reencode = false;
        p = run_video("5c 循环音频+视频copy", "t5c.mp4", o);
        CHECK("5c 时长≈6s", std::fabs(p.duration - 6.0) < 0.3);
    }
    {  // 6 循环视频(音频 8.3s,视频 6s);6b 取消重新编码也不会卡死
        VideoAudioOpts o = base;
        o.audio = P("a8.aac");
        o.mode = DurMode::AudioLoopVideo;
        Probed p = run_video("6 循环视频", "t6.mp4", o);
        CHECK("6 时长≈8.3s", std::fabs(p.duration - 8.3) < 0.4);
        o.reencode = false;
        p = run_video("6b 循环视频+取消重新编码", "t6b.mp4", o);
        CHECK("6b 时长≈8.3s", std::fabs(p.duration - 8.3) < 0.5);
    }
    {  // 7 HEVC
        VideoAudioOpts o = base;
        o.vcodec = L"libx265";
        o.preset = L"ultrafast";
        o.res_keep = false;
        o.width = L"640";
        o.height = L"360";
        Probed p = run_video("7 libx265", "t7.mp4", o);
        auto v = p.vstream();
        CHECK("7 hevc + hvc1 标签", v && v->get("codec_name") == "hevc" && v->get("codec_tag_string") == "hvc1");
        CHECK("7 640x360", v && to_i(v->get("width")) == 640 && to_i(v->get("height")) == 360);
    }
    {  // 8 拉伸 / 黑边
        VideoAudioOpts o = base;
        o.res_keep = false;
        o.width = L"640";
        o.height = L"640";
        o.pad = false;
        Probed p = run_video("8 拉伸", "t8.mp4", o);
        CHECK("8 640x640", p.vstream() && to_i(p.vstream()->get("width")) == 640 && to_i(p.vstream()->get("height")) == 640);
        o.pad = true;
        p = run_video("8b 黑边", "t8b.mp4", o);
        CHECK("8b 640x640", p.vstream() && to_i(p.vstream()->get("width")) == 640 && to_i(p.vstream()->get("height")) == 640);
    }
    {  // 10 替换已有音轨
        VideoAudioOpts o = base;
        o.video = P("with_audio.mp4");
        Probed p = run_video("10 替换音轨", "t10.mp4", o);
        CHECK("10 只有 1 条音轨", p.astreams().size() == 1);
        CHECK("10 时长≈4s(来自 mp3)", std::fabs(p.duration - 4.05) < 0.3);
    }
    {  // 11 CRF 与 faststart
        VideoAudioOpts o = base;
        o.crf = L"35";
        o.faststart = false;
        Built b;
        Probed p = run_video("11 crf=35,无 faststart", "t11.mp4", o, &b);
        CHECK("11 命令含 -crf 35、不含 faststart", arg_after(b.cmd, L"-crf") == L"35" && !has_arg(b.cmd, L"-movflags"));
        CHECK("11 输出可读", p.ok && p.vstream() != nullptr);
    }
    {  // 12 额外参数
        VideoAudioOpts o = base;
        o.extra = L"-metadata \"title=My Clip\"";  // 引号需放在参数开头(与原版规则一致)
        Built b;
        Probed p = run_video("12 额外参数", "t12.mp4", o, &b);
        CHECK("12 额外参数被带入", arg_after(b.cmd, L"-metadata") == L"title=My Clip");
        CHECK("12 输出可读", p.ok);
    }
    {  // 13 路径含空格和中文
        std::system(("mkdir -p " + sh_quote(g_dir + "/带 空格 目录")).c_str());
        std::system(("cp " + sh_quote(g_dir + "/silent.mp4") + " " + sh_quote(g_dir + "/带 空格 目录/无声 视频.mp4")).c_str());
        VideoAudioOpts o = base;
        o.video = P("带 空格 目录/无声 视频.mp4");
        Probed p = run_video("13 中文/空格路径", "带 空格 目录/结果 文件.mp4", o);
        CHECK("13 输出可读", p.ok && p.astreams().size() == 1);
    }
    {  // 14 把生成的命令显示成文本再拆回来运行(模拟用户在文本框里编辑后运行)
        VideoAudioOpts o = base;
        o.video = P("带 空格 目录/无声 视频.mp4");
        o.audio = P("a4.mp3");
        o.output = P("t14.mp4");
        std::remove((g_dir + "/t14.mp4").c_str());
        wstring shown = join_cmd(build_video_audio(o).cmd);
        int rc = run_cmd(split_cmd(shown));
        CHECK("14 文本框往返后可运行", rc == 0 && probe(o.output).ok);
    }

    // 标签页 1:图片 + 音频
    {
        ImageAudioOpts o;
        o.ffmpeg = L"ffmpeg";
        o.image = P("img.png");
        o.audio = P("a4.mp3");
        o.output = P("img.mp4");
        std::remove((g_dir + "/img.mp4").c_str());
        int rc = run_cmd(build_image_audio(o));
        CHECK("T1 ffmpeg 退出码 0", rc == 0);
        Probed p = probe(o.output);
        auto v = p.vstream();
        CHECK("T1 输出 1280x720 含音频", v && to_i(v->get("width")) == 1280 && to_i(v->get("height")) == 720 &&
                                              p.astreams().size() == 1);
        CHECK("T1 时长≈4s(-shortest)", std::fabs(p.duration - 4.05) < 0.4);
    }

    // 用真实 ffprobe 输出检查 describe_media
    {
        int rc = 0;
        std::string out = capture(std::string("ffprobe -v error -show_entries ") + probe_show_entries() + " " +
                                  sh_quote(g_dir + "/with_audio.mp4") + " 2>/dev/null", &rc);
        wstring d = describe_media(rc == 0, out, true);
        CHECK("真实 ffprobe: 视频描述", contains(d, L"640x360") && contains(d, L"25 fps") && contains(d, L"h264") && contains(d, L"含音轨"));
        out = capture(std::string("ffprobe -v error -show_entries ") + probe_show_entries() + " " + sh_quote(g_dir + "/a4.mp3") + " 2>/dev/null", &rc);
        d = describe_media(rc == 0, out, false);
        CHECK("真实 ffprobe: 音频描述", contains(d, L"mp3") && contains(d, L"44100 Hz") && contains(d, L"单声道"));
        out = capture(std::string("ffprobe -v error -show_entries ") + probe_show_entries() + " " + sh_quote(g_dir + "/img.png") + " 2>/dev/null", &rc);
        CHECK("真实 ffprobe: 图片当音频 -> 无音频流", describe_media(rc == 0, out, false) == L"(文件中没有音频流)");
        out = capture(std::string("ffprobe -v error -show_entries ") + probe_show_entries() + " /nonexistent.mp4 2>/dev/null", &rc);
        CHECK("真实 ffprobe: 不存在的文件 -> 读取失败", describe_media(rc == 0, out, true) == L"(无法读取媒体信息:文件可能已损坏或格式不受支持)");
    }
}

int main(int argc, char** argv) {
    bool unit_only = (argc > 1 && std::strcmp(argv[1], "--unit") == 0);

    test_strings();
    test_numbers();
    test_paths();
    test_image_audio_cmd();
    test_video_audio_cmd();
    test_probe_parse();

    if (!unit_only) {
        char tmpl[] = "/tmp/core_test_XXXXXX";
        const char* d = mkdtemp(tmpl);
        if (!d) {
            std::puts("无法创建临时目录");
            return 2;
        }
        g_dir = d;
        if (!make_media()) {
            std::puts("生成测试素材失败(需要 ffmpeg,且带 libx264 / libmp3lame / aac)");
            return 2;
        }
        test_integration();
        std::system(("rm -rf " + sh_quote(g_dir)).c_str());
    }

    std::printf("\n%d 项通过, %zu 项失败\n", g_pass, g_fail.size());
    for (auto& f : g_fail) std::printf("  - %s\n", f.c_str());
    return g_fail.empty() ? 0 : 1;
}
