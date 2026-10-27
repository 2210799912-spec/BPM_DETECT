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

// 校准(2026-10-20 二次校准): 板上实测 150BPM 稳定锁定 176.9、120 真实间隔~23150
// 样本, 均表明真实采样率约 46.3kHz(非 54335/60418)。反推:
//   150->176.9  => FS = 54335*150/176.9 = 46078
//   120 间隔 23043~23309 => FS = 46086~46618
// 取 FS=46300。此前 54335 基于被污染的老 166.8 推断, 是错的, 已废弃。
#define FS_HZ            46300            // 采样率基准 (板上实测校准 ~46.3k)
#define BPM_CONST        2778000          // FS_HZ * 60

// --- 能量包络窗 (短窗移动平均), 2^ENV_SHIFT 个样本 ---
#define ENV_SHIFT        10
#define ENV_WIN          (1 << ENV_SHIFT)

// 修正(2026-10-20): SAMPLE_SHIFT=0 + ADAPT_FACTOR + BASE_SHIFT=17 + 局部峰判别。
// 2026-10-22 二次修正(忠实镜像扫描): 触发条件等价 瞬时包络env_sum > 慢均base×ADAPT。
// 旧值 ADAPT=4 阈值过低, 噪音/静音振荡尖峰穿越 -> 静音也报 BPM~180 假值(镜像+板载同现)。
// 扫描 ADAPT∈[4..512], 狠噪音(振荡2000+噪400)下:
//    - ADAPT=16: 静音仅 1 个杂散拍(不构成假BPM), 干净 60/90/120/150/180 全准, 弱拍60仍检出;
//    - ADAPT≥24: 开始丢掉弱拍60。故取 ADAPT=16。
#define SAMPLE_SHIFT     0
// ADAPT_FACTOR 2026-10-24 板上实测校准 16->4:
//   板上 0x60(env) 实测真实节拍 tick 峰值≈9e7, 慢均值 base≈1e7。
//   原值16使 THR=1.6e8 > tick峰值9e7 => 每个真拍都被阈值削掉(cnt=0,BPM=0,全测不出)。
//   降到4: THR≈4e7, 真拍(9e7>4e7)可触发; 静音由 SILENCE_E_MIN(6.7e7)>静音p99(2.3e7) 兜底,
//   故不会因阈值降低而放静音假拍。 若要更抗环境抬升, 可改3~6。
#define ADAPT_FACTOR     4                // 2026-10-24: 16->4 (板上实测 THR 削拍, 降基数放过真拍)
#define BASE_SHIFT       17

// --- 不应期: 两次有效峰之间最少样本数, 对应最高可测 BPM (~180) ---
// 2026-10-21 板载实测: 每个真拍后紧跟一个 ~12.5k 样本的伪子峰, 旧值 11573(BPM_CONST/240)
// 会让它穿过去造成 2~3 倍过触(120/150 全被压成 ~113)。抬到 15433(BPM_CONST/180,
// 与 BPM_LO_INTERVAL 对齐: 伪子峰间隔<REFRACTORY 被掐, 真实 120/150/180 间隔不受影响)。
#define REFRACTORY       15433           // BPM_CONST/180 (2026-10-21 抑制伪子峰过触)

// ================= BPM 结算: 区间众数直方图 (emode, 等权计数) =================
// 增强(2026-10-19): 相比原"最近峰-峰间隔历史平均"(对音乐 off-beat 打击乐会误检
// 翻倍), 改为 interval 直方图 + 区间众数:
//    - 只统计落在 [60BPM,180BPM) 对应 interval 区间的峰(半拍=>>180BPM被丢弃);
//    - 每峰给所在桶投一票(等权 +1 —— 2026-10-20 由"峰能量加权"改为等权:
//      真实板载 loud 场景下, 极少数巨大能量杂散峰靠 env_sum 权会瞬间压过多数真实拍,
//      导致 120 被拉到 146.6; 等权让"多数真实拍"必然胜出);
//    - 输出 BPM = 累计票数最高的桶中心 interval 对应的 BPM。
// 时序优化(newip_optimized, 2026-10-19 综合 SLACK=-6.51ns 后):
//    - BPM 用 64 桶 ROM 查找表 (bpm_table) 取代浮点除法 BPM_CONST/center,
//      移除 fdiv 长延迟单元, 关键路径显著缩短;
//    - hist 位宽 40->32bit, 缩短 barrel-shifter/加法/比较组合路径;
//    - II 请求 2->3, 匹配 hist/last_vote 双 BRAM 端口实际间隔, 消除 II Violation。
#define BPM_LO_INTERVAL  (BPM_CONST / 180)   // 15433  (对应 180 BPM)
#define BPM_HI_INTERVAL  (BPM_CONST / 60)    // 46300  (对应 60 BPM)
#define HIST_BINS        64                  // 直方图桶数
#define HIST_BINW        640
#define BPM_SCALE        256                 // 定点 BPM 放大倍数 (Q8, 见 bpm_table)
#define TAU_SHIFT        14                  // 响应速度(2026-10-23 17->14): O(1) 指数遗忘半衰期由~2.8s降到~0.35s
                                             //   (2^14/46300≈0.35s), 让旧节拍票数快速衰减, 切换节奏时新 BPM 几拍内当选
#define WATCHDOG_SAMPLES (2 * BPM_HI_INTERVAL)   // 无拍看门狗 ~2s(2026-10-23): 超过仍无新拍=>静音, 输出清零

// 2026-10-23 每拍绝对能量下限(静音/滤噪门): 仅当某拍瞬时窗能量 env_sum(即 es_d1)
//   > SILENCE_E_MIN 才判为有效节拍; 静音/低幅值噪音的局部峰达不到该下限 => 不触发假拍。
//   静音期无假拍后, §4 看门狗(约2s无新拍)才会真正清零 BPM, 静音时显示归位。
//   注意: 必须按"拍峰值"而非慢均值 base 门控(拍是稀疏的, base 会低估)。
//   校准: 上板读 0x60(env_sum)在"静音时峰值"与"真实拍时"的值, 把 SILENCE_E_MIN 设在
//   两者之间(≈静音峰值×3)。默认按 C-sim 测试音标定, 板上按实测微调。
#define SILENCE_E_MIN    (1 << 26)            // 默认 ~6.7e7; 跨环境按 0x60 静音读数校准

struct beat_event_t {
    ap_uint<1>  pulse;
    ap_uint<32> interval;
};

// ================= AXI-Lite (s_axilite, bundle=CTRL) 寄存器数组接口 =================
//   数组做了 ARRAY_PARTITION complete, 生成 IP 的 AXI-Lite 内存映射变为逐独立寄存器
//   (自 0x10 起每元素一格):
//     0x10 regs[0] : total_samples —— 仅 C 仿真用有界退出开关(硬件忽略)
//     0x40 regs[6] : bpm_value    —— 实时 BPM (IEEE754 float)
//     0x50 regs[7] : debug_cnt    —— 计入直方图的峰(节拍)累计次数
//     0x60 regs[8] : energy_ratio —— 当前窗包络能量 env_sum 低32位

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
