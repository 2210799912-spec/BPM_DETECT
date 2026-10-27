// ============================================================
// bpm_detect.cpp —— Pan-Tompkins 阈值检测 + emode 节拍众数 (综合时序修复版)
// ------------------------------------------------------------
// 单进程无限流式 (ap_ctrl_none), 一个有界 for 大循环顺序完成:
//   unpack -> Pan-Tompkins -> emode BPM -> AXI-Lite 寄存器
//
// 综合时序修复 (针对 2026-10-19 综合报告 Fmax=72.39MHz / 关键路径13.8ns):
//   [1] 位宽收窄: env_win/env_sum/base 64bit->48bit, thr 用 ap_int<52>。
//       包络实际最大 ~2^42(16bit差分dx*dx<=2^32, 1024窗和), 48bit 足够,
//       但 sub/add/icmp 的 64bit 长链缩短约 1ns/级, 关键路径显著下降。
//   [2] hist/last_vote 由 BRAM 改寄存器数组 (complete partition, 各64深):
//       消除 BRAM store 延迟并进关键路径尾部, 也去掉"双BRAM端口"II约束。
//   [3] regs 数组 ARRAY_PARTITION complete: s_axilite 多写地址不再受
//       单端口内存限制, 消除 regs_addr_2_write 的 II Violation。
//   算法数值与 emode 版完全一致 (48bit 不截断), C-sim 结果不变。
// ============================================================
#include "bpm_detect.h"

// --- 64 桶 BPM 查找表 (Q8 定点: 值 = BPM*256) ---
// 桶 i 中心 interval = BPM_LO_INTERVAL + i*HIST_BINW + HIST_BINW/2,
//   bpm_table[i] = BPM_CONST*BPM_SCALE / center_i  (整数, 预计算 ROM)
// bin63 永远不会收到投票 (interval<=BPM_HI_INTERVAL 对应最大 bin=62), 置 0。
static const ap_uint<16> bpm_table[HIST_BINS] = {
    45360, 43984, 42689, 41468,
    40315, 39224, 38191, 37211,
    36280, 35394, 34551, 33747,
    32979, 32246, 31544, 30873,
    30229, 29612, 29019, 28450,
    27902, 27376, 26868, 26379,
    25908, 25453, 25014, 24590,
    24180, 23783, 23399, 23028,
    22668, 22319, 21981, 21652,
    21334, 21024, 20724, 20432,
    20148, 19872, 19603, 19342,
    19087, 18839, 18597, 18362,
    18132, 17908, 17690, 17477,
    17269, 17065, 16867, 16673,
    16483, 16298, 16117, 15940,
    15766, 15597, 15431, 0
};

void bpm_detect(
    axis_stream &audio_in,
    axis_stream &audio_out,
    volatile uint32_t regs[REG_DEPTH]
) {
    #pragma HLS INTERFACE axis port=audio_in
    #pragma HLS INTERFACE axis port=audio_out
    #pragma HLS INTERFACE s_axilite port=regs bundle=CTRL
    #pragma HLS INTERFACE ap_ctrl_none port=return

    // regs 是 AXI-Lite 接口数组: 每样本写 BPM/DEBUG/RATIO 多地址,
    // 不加 partition 会被当成单端口内存导致 II 冲突 -> complete 拆成独立寄存器。
    #pragma HLS ARRAY_PARTITION variable=regs complete dim=1

    ap_uint<32> local_stop;
#ifndef __SYNTHESIS__
    local_stop = regs[REG_TOTAL];
#else
    local_stop = 0xFFFFFFFFu;
#endif

    // ---- Pan-Tompkins 状态 (位宽收窄到48bit, 实际够且缩短关键路径) ----
    // 显式清零: 保证上电/填充期 BRAM 内容确定, 避免 env_sum 减到脏值引起
    // 无符号下溢而永久锁死阈值 (缺陷2 加固)。`= {}` 会让 HLS 综合时生成 BRAM 初始化,
    // 无需 init_val 参数 (该参数不受 ram_1p 指令支持)。
    static ap_uint<48> env_win[ENV_WIN] = {};   // 环形能量窗 (1024 深, 必须 BRAM)
    #pragma HLS BIND_STORAGE variable=env_win type=ram_1p impl=bram
    static ap_uint<48> env_sum    = 0;
    static ap_uint<32> env_idx    = 0;
    static ap_int<32>  prev_x     = 0;
    static ap_uint<32> last_peak  = 0;
    static ap_uint<32> cnt        = 0;
    static ap_int<48>  base       = 0;

    // ---- emode 直方图状态 (寄存器数组, 消除 BRAM store 时序并进关键路径) ----
    static ap_uint<32> hist[HIST_BINS];       // 每桶累计峰能量
    #pragma HLS ARRAY_PARTITION variable=hist complete dim=1
    static ap_uint<32> last_vote[HIST_BINS];  // 每桶上次投票样本计数
    #pragma HLS ARRAY_PARTITION variable=last_vote complete dim=1
    static ap_uint<32> best_energy = 0;
    static ap_uint<16> best_bin    = 0;
    static ap_uint<32> update_cnt  = 0;
    static float       smoothed_bpm = 0.0f;   // 输出 BPM (IEEE754 float)

    const ap_uint<48> IDX_MASK = (ap_uint<48>)(ENV_WIN - 1);

#ifndef __SYNTHESIS__
    #pragma HLS STREAM variable=audio_in  depth=262144
    #pragma HLS STREAM variable=audio_out depth=262144
#endif

    // II=2: 位宽收窄+寄存化解锁端口后, 尽量压到 2 周期吞吐 (100MHz/2=50M样本/s >> 60k)。
    // 若综合报 II 冲突再退回 II=3, 60k 采样对 II 完全无压力。
    BPM_LOOP: for (ap_uint<32> loop_cnt = 0; loop_cnt < local_stop; loop_cnt++) {
        #pragma HLS PIPELINE II=2

        // ================= 1. unpack + 透传 =================
        axis_sample in_word = audio_in.read();
        audio_out.write(in_word);
        ap_int<32> x = (ap_int<32>)((ap_int<24>)in_word.data(23, 0));

        // ================= 2. Pan-Tompkins 特征提取 =================
        ap_int<16> xs = (ap_int<16>)(x >> SAMPLE_SHIFT);
        ap_int<32> dx = (ap_int<32>)xs - prev_x;
        prev_x        = (ap_int<32>)xs;
        // dx 为两16bit样本之差(最大约±65535), dx*dx 可达 ~4.29e9, 已逼近32bit
        // 无符号上限——用48bit承接防溢出, 避免极端尖峰污染 env_sum。
        ap_uint<48> e = (ap_uint<48>)(ap_int<48>)dx * (ap_int<48>)dx;

        ap_uint<32> idx = (ap_uint<32>)(env_idx & IDX_MASK);
        // 填充期保护: 前 ENV_WIN 个样本窗口未填满, 只加不减,
        // 避免上电时对未定义(或极值)旧值做减法导致 env_sum 下溢。
        if (cnt < (ap_uint<32>)ENV_WIN) {
            env_win[idx] = (ap_uint<48>)e;
            env_sum = env_sum + (ap_uint<48>)e;
        } else {
            env_sum = env_sum - env_win[idx];
            env_win[idx] = (ap_uint<48>)e;
            env_sum = env_sum + (ap_uint<48>)e;
        }
        env_idx = env_idx + 1;

        base += ((ap_int<48>)env_sum - base) >> BASE_SHIFT;
        ap_int<52> thr = (ap_int<52>)base * (ap_int<52>)ADAPT_FACTOR;
        if (thr < 0) thr = 0;

        beat_event_t event;
        event.pulse    = 0;
        event.interval = 0;
        if (env_sum > (ap_uint<48>)thr && (cnt - last_peak) >= REFRACTORY) {
            event.pulse    = 1;
            event.interval = cnt - last_peak;
            last_peak      = cnt;
        }

        // ================= 3. BPM 结算 (emode, ROM 查表定点) =================
        if (event.pulse == 1 &&
            event.interval >= (ap_uint<32>)BPM_LO_INTERVAL &&
            event.interval <= (ap_uint<32>)BPM_HI_INTERVAL) {
            ap_uint<16> bin = (ap_uint<16>)((event.interval - BPM_LO_INTERVAL) / HIST_BINW);
            if (bin < (ap_uint<16>)HIST_BINS) {
                // O(1) 指数遗忘: age=距上次该桶投票样本数, 按 age>>TAU_SHIFT 减半
                ap_uint<32> age = cnt - last_vote[bin];
                ap_uint<32> halves = (age >> TAU_SHIFT);
                ap_uint<32> decayed = (halves >= 32) ? (ap_uint<32>)0
                                                     : (ap_uint<32>)(hist[bin] >> halves);
                hist[bin] = decayed + (ap_uint<32>)(env_sum >> ENERGY_SHIFT);
                last_vote[bin] = cnt;
                update_cnt++;
                if (hist[bin] > best_energy) {              // 增量追踪众数桶
                    best_energy = hist[bin];
                    best_bin    = bin;
                    smoothed_bpm = (float)bpm_table[best_bin] * (1.0f / (float)BPM_SCALE);
                }
            }
        }

        // ================= 4. 写回寄存器 =================
        union { float f; uint32_t u; } conv;
        conv.f = smoothed_bpm;
        regs[REG_BPM]   = conv.u;
        regs[REG_DEBUG] = update_cnt;
        regs[REG_RATIO] = (ap_uint<32>)(env_sum & 0xFFFFFFFFul);

        cnt++;
    }
}
