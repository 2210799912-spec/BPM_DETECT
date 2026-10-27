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

// 兜底定义: 防止 Linux 侧工程用旧版 bpm_detect.h 时(其无 SILENCE_E_MIN)导致未声名。
// 新版 .h 已含同值定义, #ifndef 保证二者不冲突。
#ifndef SILENCE_E_MIN
#define SILENCE_E_MIN (1 << 26)
#endif

// --- 64 桶 BPM 查找表 (Q8 定点: 值 = BPM*256), 校准 FS=46300 ---
// 桶 i 中心 interval = BPM_LO_INTERVAL + i*HIST_BINW + HIST_BINW/2,
//   bpm_table[i] = BPM_CONST*BPM_SCALE / center_i  (整数, 预计算 ROM)
// interval 上限 BPM_HI_INTERVAL(60BPM) 对应最大活跃 bin≈48, 高桶极少命中。
static const ap_uint<16> bpm_table[HIST_BINS] = {
    45144, 43382, 41752, 40240, 38834, 37522, 36297, 35148,
    34071, 33057, 32102, 31201, 30348, 29542, 28777, 28050,
    27359, 26702, 26075, 25478, 24906, 24360, 23838, 23337,
    22857, 22396, 21954, 21529, 21119, 20725, 20346, 19980,
    19627, 19286, 18957, 18639, 18332, 18034, 17746, 17467,
    17197, 16935, 16681, 16434, 16194, 15962, 15736, 15516,
    15302, 15094, 14892, 14695, 14503, 14316, 14134, 13957,
    13784, 13615, 13450, 13289, 13132, 12979, 12829, 12682
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
    // 局部峰判别延迟链: 记录包络的最近两拍值与阈值, 判定"上一拍包络为峰顶"。
    static ap_uint<48> es_d1      = 0;   // env_sum[n-1]
    static ap_uint<48> es_d2      = 0;   // env_sum[n-2]
    static ap_uint<48> thr_d1     = 0;   // 上一拍阈值(钳位非负)

    // ---- emode 直方图状态 (寄存器数组, 消除 BRAM store 时序并进关键路径) ----
    static ap_uint<32> hist[HIST_BINS];       // 每桶累计等权票数
    #pragma HLS ARRAY_PARTITION variable=hist complete dim=1
    static ap_uint<32> last_vote[HIST_BINS];  // 每桶上次投票样本计数
    #pragma HLS ARRAY_PARTITION variable=last_vote complete dim=1
    static ap_uint<32> best_energy = 0;
    static ap_uint<16> best_bin    = 0;
    static ap_uint<32> last_best_cnt = 0;   // 设置 best_energy 时的样本计数(用于衰减)
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
        #pragma HLS PIPELINE II=4    // 2026-10-22: II 2->4, 放松环回依赖链, 压关键路径(slack -0.30->正)

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
        // 局部峰判别 (2026-10-20): 仅当上一拍包络 es_d1 为峰顶(>=前一拍, >当前拍)且
        // 超过上一拍阈值、且距上一峰已过不应期时, 判定为一次有效节拍触发, 时刻=cnt-1。
        // 相比"当前 env_sum > 阈值即触发": 包络长期高于阈值(噪声/强拍)时只触发一次,
        // 从机制上杜绝按抑制期上限连发(板上 240 空转根因); 也不会新增任何假拍。
        // 局部峰判别 (2026-10-20): 仅当上一拍包络 es_d1 为峰顶(>=前一拍, >当前拍)且
        // 超过上一拍阈值、且距上一峰已过不应期时, 判定为一次有效节拍触发, 时刻=cnt-1。
        // 相比"当前 env_sum > 阈值即触发": 包络长期高于阈值(噪声/强拍)时只触发一次,
        // 从机制上杜绝按抑制期上限连发(板上 240 空转根因); 也不会新增任何假拍。
        // 2026-10-23: 追加静音门 es_d1 > SILENCE_E_MIN, 低幅值噪音峰过不了下限=>不触发。
        if ((cnt - 1 - last_peak) >= REFRACTORY &&
            es_d1 > (ap_uint<48>)SILENCE_E_MIN &&
            es_d1 > thr_d1 &&
            es_d1 >= es_d2 &&
            es_d1 > env_sum) {
            event.pulse    = 1;
            event.interval = (cnt - 1) - last_peak;
            last_peak      = cnt - 1;
        }
        // 延迟链移位: 当前包络/阈值进入上一拍位置(供下一次判定)
        es_d2  = es_d1;
        es_d1  = env_sum;
        thr_d1 = (ap_uint<48>)thr;   // thr 上文已钳位非负

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
                // 等权计数(2026-10-20): 每有效区间投 +1 票, 替代原 env_sum 能量加权,
                // 避免极少数巨大能量杂散峰压过多数真实拍(板上 120 被拉到 146.6 的根因)。
                hist[bin] = decayed + (ap_uint<32>)1;
                last_vote[bin] = cnt;
                update_cnt++;
                // 历史峰值衰减(2026-10-21): 让 best_energy 也按 TAU_SHIFT 年龄指数衰减,
                // 否则某桶一旦累计到历史最高票, best_bin 永久锁死, BPM 冻结(板上定格 176.3 因)。
                // 每有效投票时先衰减旧 best_energy, 再比较, 使众数能随时间迁移到当前节奏。
                ap_uint<32> b_age     = cnt - last_best_cnt;
                ap_uint<32> b_halves  = (b_age >> TAU_SHIFT);
                ap_uint<32> decayed_best = (b_halves >= 32) ? (ap_uint<32>)0
                                                            : (best_energy >> b_halves);
                if (hist[bin] > decayed_best) {     // 当前桶成为新的众数
                    best_energy     = hist[bin];
                    best_bin        = bin;
                    last_best_cnt   = cnt;
                    smoothed_bpm    = (float)bpm_table[best_bin] * (1.0f / (float)BPM_SCALE);
                } else {
                    best_energy     = decayed_best; // 旧众数仍领先但已按年龄衰减
                }
            }
        }

        // ==== 4. 无拍看门狗 + 写回寄存器 ====
        // 2026-10-23 恢复清零(配合 §2 静音门): 静音假拍已被 SILENCE_E_MIN 挡住, 故
        //   距上次节拍超过 WATCHDOG_SAMPLES(约2s)无新拍即可判定静音并清零输出,
        //   这样静音时 BPM 能归位(体现"没在响"), 不会因假拍瞬间又被顶上去。
        //   (此前删除本块会导致静音永久锁死最后值, 无法判断BPM变化。)
        if ((cnt - last_peak) > (ap_uint<32>)WATCHDOG_SAMPLES) {
            smoothed_bpm = 0.0f;
            best_energy  = 0;
            best_bin     = 0;
        }
        union { float f; uint32_t u; } conv;
        conv.f = smoothed_bpm;
        regs[REG_BPM]   = conv.u;
        regs[REG_DEBUG] = update_cnt;
        regs[REG_RATIO] = (ap_uint<32>)(env_sum & 0xFFFFFFFFul);

        cnt++;
    }
}
