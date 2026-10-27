// tb.cpp (emode 增强版) —— bpm_detect C 仿真测试平台
// ------------------------------------------------------------
// 聚焦验证 emode(能量加权区间众数)对 "音乐 off-beat 打击乐" 的鲁棒性:
//   场景含 每拍 kick(底鼓) + 2 个非节拍 accent(hi-hat/snare, 位置固定 8 分音,
//   幅度 = kick 的 25%), 这正是原历史平均版误检翻倍(BPM 240 / ~170)的场景。
//   本版应读出 ≈120 BPM。
// 信号构造(24-bit 补码, 静音=0, 等价板载 decode):
//   底噪 ±30 + 每拍指数衰减 kick + 每拍两个 off-beat accent(0.4/0.65 拍)。
// 判据: BPM ∈ (110,130), 计入峰数>=4, 透传一致 -> TEST PASS。
// 注意: 只跑 1 个 case, 因为 IP 内 hist/last_vote 为 static, C-sim 多次调用
//       之间不会复位, 连续测多个不同 BPM 会互相污染直方图。多 BPM 精度由
//       python 镜像(mirror_final_emode.py, 独立进程逐 case)验证, 全网格 0 失败。
// 用法: HLS 工程 source 目录放 bpm_detect.h/.cpp/tb.cpp, `vitis_hls`/`v++ csim` 即可。
// ============================================================
#include "bpm_detect.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>

// ---- 构造信号: 底噪 + 每拍衰减 kick + 每拍 off-beat accent ----
// 返回 24-bit 补码样本 (silence=0). beat_amp: kick 峰值; acc_frac: accent 相对 kick 幅度.
static std::vector<axis_sample> make_music(double bpm, int nsamp,
                                          int beat_amp, int noise,
                                          int nacc, double acc_frac,
                                          int jit, int seed = 42) {
    std::vector<int64_t> sig(nsamp);
    std::srand(seed);
    for (auto &v : sig) v = (std::rand() % (2 * noise + 1)) - noise;  // 底噪 ±noise

    int BEAT = (int)(FS_HZ * 60.0 / bpm);
    auto peak_at = [&](int at, int64_t amp, double decay) {
        for (int k = 0; k < 600; k++) {
            int idx = at + k;
            if (idx >= nsamp) break;
            int64_t a = (int64_t)(amp / (1.0 + decay * k));
            if (a < 1) break;
            sig[idx] = (sig[idx] + a) & 0xFFFFFF;
        }
    };
    for (int start = BEAT / 2; start < nsamp; start += BEAT) {
        int tj = (std::rand() % (2 * jit + 1)) - jit;
        peak_at(start + tj, beat_amp, 0.25);        // kick (每次整拍)
        // 固定 8 分音符位置 hi-hat/snare(真实音乐最常见, 非随机) + 少量抖动
        for (int a = 0; a < nacc; a++) {
            double pos = (a == 0) ? 0.40 : 0.65;    // 0.4拍 / 0.65拍
            int at = start + (int)(BEAT * pos) + (std::rand() % 301) - 150;
            if (at < start || at >= nsamp) continue;
            double f = acc_frac * (0.7 + 0.6 * (std::rand() % 100) / 100.0);
            peak_at(at, (int64_t)(beat_amp * f), 0.6);
        }
    }
    std::vector<axis_sample> in(nsamp);
    for (int n = 0; n < nsamp; n++) {
        axis_sample s; s.data = 0; s.keep = 0xF; s.strb = 0xF;
        s.last = 0; s.user = 0; s.id = 0; s.dest = 0;
        s.data = (ap_uint<32>)((uint64_t)(sig[n] & 0xFFFFFF));
        in[n] = s;
    }
    in[nsamp - 1].last = 1;
    return in;
}

int main() {
    std::printf("=== emode bpm_detect C-sim (8s, true-I2S twos-complement) ===\n");
    std::printf("场景: 120BPM + 每拍2个 off-beat 8分音 accent(25%% kick) -- 原历史平均版误测翻倍, 本版应≈120\n");

    const double TARGET = 120.0;
    const int NSAMP = FS_HZ * 8;                 // 8 秒 @60kHz (>= 启动收敛)
    std::vector<axis_sample> in = make_music(TARGET, NSAMP, 0x001400, 30, 2, 0.25, 6, 7);

    hls::stream<axis_sample> audio_in("audio_in");
    hls::stream<axis_sample> audio_out("audio_out");
    for (auto &s : in) audio_in.write(s);

    uint32_t regs[REG_DEPTH] = {0};
    regs[REG_TOTAL] = (uint32_t)NSAMP;           // C-sim 有界退出开关
    bpm_detect(audio_in, audio_out, regs);

    // 透传一致性
    bool tx_ok = true; long tx = 0;
    while (!audio_out.empty()) {
        axis_sample o = audio_out.read();
        if (tx < NSAMP && o.data != in[tx].data) tx_ok = false;
        tx++;
    }

    float b = 0.0f; { union { float f; std::uint32_t u; } cv; cv.u = regs[REG_BPM]; b = cv.f; }
    unsigned dbg = regs[REG_DEBUG];
    bool pass = (b > TARGET - 10.0f && b < TARGET + 10.0f) && (dbg >= 4) && tx_ok && (tx == NSAMP);

    std::printf("target=%g BPM, read=%g BPM, peaks=%u, tx=%ld/%d, 透传一致=%s\n",
                TARGET, b, dbg, tx, NSAMP, tx_ok ? "Y" : "N");
    std::printf("\n===== %s =====\n", pass ? "TEST PASS" : "TEST FAIL");
    return pass ? 0 : 1;
}
