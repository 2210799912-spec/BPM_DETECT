// ============================================================
// tb.cpp —— Pan-Tompkins bpm_detect C 仿真测试平台
// ------------------------------------------------------------
// 构造 3 秒、120 BPM 的周期性鼓点信号:
//   - 底噪: ±32 LSB 伪随机 (模拟环境/自身噪声)
//   - 鼓点: 每 0.5s (30209 样本) 一个 50 样本宽的方波脉冲, 幅度 0x100000
// 期望: sample_process 在差分+平方+包络+自适应阈值+不应期下,
//       检测出 6 个峰, 峰-峰间隔≈30209, BPM≈120.
// 判据: BPM∈(100,145) 且峰计数>=4 -> PASS
//
// 用法: 放进 HLS 工程 source 目录, `csim` 即可 (需含 bpm_detect.h/.cpp)
// 说明(阈值自锁回归): 若 BASE_SHIFT 过小(旧12)+ADAPT 过大(旧6), base
//       会追平鼓点能量级, thr=6*base 恒高于 env_sum, 导致本 TB 峰数=0、
//       BPM=0 且 pass=FAIL。当前头文件已改为 BASE_SHIFT=17/ADAPT=3,
//       本 TB 天然是自锁回归的判据。
// ============================================================
#include "bpm_detect.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>

int main() {
    const int NSAMP = FS_HZ * 3;          // 3 秒 @60418Hz
    const double TARGET_BPM = 120.0;
    const int BEAT_SAMPLES = (int)(FS_HZ * 60.0 / TARGET_BPM); // ≈30209
    const int PULSE_W      = 50;                                  // 鼓点脉宽
    const int BEAT_AMP     = 0x100000;                            // 鼓点幅度(24bit域)

    std::printf("Pan-Tompkins bpm_detect C-sim @ %.0f Hz, %d samples (%.2fs)\n",
                (double)FS_HZ, NSAMP, NSAMP / (double)FS_HZ);
    std::printf("期望鼓点间隔 %d 样本 -> %g BPM\n\n", BEAT_SAMPLES, TARGET_BPM);

    // ---- 构造输入信号 ----
    std::vector<axis_sample> in(NSAMP);
    std::srand(42);
    for (int n = 0; n < NSAMP; n++) {
        axis_sample s;
        s.data = 0; s.keep = 0xF; s.strb = 0xF;
        s.last = 0; s.user = 0; s.id = 0; s.dest = 0;
        // 底噪: ±32 LSB
        s.data = (ap_uint<32>)(((std::rand() % 65) - 32) & 0xFFFFFF);
        in[n] = s;
    }
    // 叠加鼓点: 每 BEAT_SAMPLES 一个方波脉冲, 幅度 0x100000
    int nbeats = 0;
    for (int b = 0; b * BEAT_SAMPLES < NSAMP; b++) {
        int start = b * BEAT_SAMPLES + 8;
        for (int k = 0; k < PULSE_W && (start + k) < NSAMP; k++) {
            in[start + k].data = (ap_uint<32>)BEAT_AMP;
        }
        nbeats++;
    }
    in[NSAMP-1].last = 1;
    std::printf("注入 %d 个鼓点脉冲\n", nbeats);

    // ---- 输入/输出流 ----
    hls::stream<axis_sample> audio_in("audio_in");
    hls::stream<axis_sample> audio_out("audio_out");
    for (auto &s : in) audio_in.write(s);

    // ---- 寄存器 ----
    uint32_t regs[REG_DEPTH] = {0};
    regs[REG_TOTAL] = (uint32_t)NSAMP;  // C-sim 有界退出开关

    // ---- 运行 ----
    bpm_detect(audio_in, audio_out, regs);

    // ---- 检查透传完整性 ----
    bool tx_ok = true;
    long tx = 0;
    while (!audio_out.empty()) {
        axis_sample o = audio_out.read();
        if (tx < NSAMP && o.data != in[tx].data) tx_ok = false;
        tx++;
    }
    std::printf("透传样本数=%ld (期望 %d), 内容一致=%s\n", tx, NSAMP, tx_ok ? "YES" : "NO");

    // ---- 读取结果 ----
    float bpm = 0.0f;
    { union { float f; std::uint32_t u; } cv; cv.u = (std::uint32_t)regs[REG_BPM]; bpm = cv.f; }
    unsigned dbg = (unsigned)regs[REG_DEBUG];
    std::printf("BPM=%g  debug_cnt(峰数)=%u  ratio(末窗)=%u\n",
                bpm, dbg, (unsigned)regs[REG_RATIO]);

    // ---- 判据 ----
    bool pass = (bpm > 100.0f && bpm < 145.0f) && (dbg >= 4) && tx_ok;
    std::printf("\n===== %s =====\n", pass ? "TEST PASS" : "TEST FAIL");
    return pass ? 0 : 1;
}
