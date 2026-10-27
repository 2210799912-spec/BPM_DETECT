#ifndef BPM_DETECT_H
#define BPM_DETECT_H

#include <ap_int.h>
#include <hls_stream.h>
#include <ap_axi_sdata.h>
#include <cstdint>

// ================= 数据类型定义 =================
typedef ap_axis<32, 1, 1, 1> axis_sample;
typedef hls::stream<axis_sample> axis_stream;

// ================= Pan-Tompkins 自适应阈值峰值检测参数 =================
// 算法链路: 差分(高通/去趋势) -> 平方(能量, 强化瞬态) -> 短窗移动平均包络(~17ms)
//           -> 自适应阈值(mean*ADAPT_FACTOR) -> 不应期 -> 峰-峰间隔 -> BPM
// 与 ECG Pan-Tompkins QRS 检测同构, 可迁移到心率/搏动检测。

#define FS_HZ            60418            // 采样率基准 (与 ADAU1761 codec PLL 帧率一致)
#define BPM_CONST        3625080          // FS_HZ * 60

// --- 能量包络窗 (短窗移动平均), 2^ENV_SHIFT 个样本 ---
#define ENV_SHIFT        10
#define ENV_WIN          (1 << ENV_SHIFT)

// 修正(2026-10-19): SAMPLE_SHIFT=0(不右移) + ADAPT_FACTOR=2, BASE_SHIFT=17
// 避免小信号下能量贴底噪+启动期虚峰导致的 BPM 恒定 240 / 自锁。
#define SAMPLE_SHIFT     0
#define ADAPT_FACTOR     2
#define BASE_SHIFT       17

// --- 不应期: 两次有效峰之间最少样本数, 对应最高可测 BPM (~240) ---
#define REFRACTORY       15104

// ================= BPM 结算: 能量加权区间众数 (emode) =================
// 增强(2026-10-19): 相比原"最近峰-峰间隔历史平均"(对音乐 off-beat 打击乐会误检
// 翻倍), 改为 interval 直方图 + 峰能量加权众数:
//    - 只统计落在 [60BPM,180BPM) 对应 interval 区间的峰(半拍=>>180BPM被丢弃);
//    - 每峰按其触发时包络能量 env_sum 给所在桶加权(kick 能量 >> accent, 更占优);
//    - 输出 BPM = 累计能量最高的桶中心 interval 对应的 BPM。
// 时序优化(newip_optimized, 2026-10-19 综合 SLACK=-6.51ns 后):
//    - BPM 用 64 桶 ROM 查找表 (bpm_table) 取代浮点除法 BPM_CONST/center,
//      移除 fdiv 长延迟单元, 关键路径显著缩短;
//    - hist 位宽 40->32bit, 缩短 barrel-shifter/加法/比较组合路径;
//    - II 请求 2->3, 匹配 hist/last_vote 双 BRAM 端口实际间隔, 消除 II Violation。
#define BPM_LO_INTERVAL  (BPM_CONST / 180)   // 20139  (对应 180 BPM)
#define BPM_HI_INTERVAL  (BPM_CONST / 60)    // 60418  (对应 60 BPM)
#define HIST_BINS        64                  // 直方图桶数
#define HIST_BINW        640
#define ENERGY_SHIFT     8                   // 峰能量右移, 防 32bit 桶累加溢出
#define BPM_SCALE        256                 // 定点 BPM 放大倍数 (Q8, 见 bpm_table)
#define TAU_SHIFT        17                  // 指数遗忘半衰期 ~2.2s (O(1) 变量右移)

struct beat_event_t {
    ap_uint<1>  pulse;
    ap_uint<32> interval;
};

// ================= AXI-Lite (s_axilite, bundle=CTRL) 寄存器数组接口 =================
//   s_axilite 数据区从 0x10 起, 每元素 4 字节:
//     0x10 regs[0] : total_samples —— 仅 C 仿真用有界退出开关(硬件忽略)
//     0x28 regs[6] : bpm_value    —— 实时 BPM (IEEE754 float)
//     0x2C regs[7] : debug_cnt    —— 计入直方图的峰(节拍)累计次数
//     0x30 regs[8] : energy_ratio —— 当前窗包络能量 env_sum 低32位
#define REG_TOTAL       0
#define REG_BPM         6
#define REG_DEBUG       7
#define REG_RATIO       8
#define REG_DEPTH       9

// 顶层函数声明 (freerun 流式: ap_ctrl_none, 持续消费 audio_in -> 透传 audio_out)
void bpm_detect(
    axis_stream &audio_in,
    axis_stream &audio_out,              // 透传给 DMA 的音频流
    volatile uint32_t regs[REG_DEPTH]    // AXI-Lite 寄存器
);

#endif
