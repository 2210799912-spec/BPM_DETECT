# BPM_DETECT：基于 PYNQ-Z2 的实时音频 BPM 检测系统

在 Xilinx PYNQ-Z2（Zynq-7020, xc7z020clg400-1）上实现的实时音频节拍检测（BPM）系统。
板载 ADAU1761 codec 将麦克风/线路音频数字化，PL 侧经 AXI 数据通路与 AXI-DMA 连续搬运到
DDR，PS 端用 Python/NumPy 完成 BPM 检测与实时双波形可视化。

## 核心思路

- **软硬协同**：PL 专职"搬运 + 检测"（FIFO / 自定义字数闸门 / AXI-DMA / bpm_detect IP），
  PS 专职"计算 + 可视化"，二者解耦。
- **算法**：差分 → 平方 → 短窗包络 → 自适应阈值 → 不应期 → 峰-峰间隔，
  以区间众数（等权投票 + 指数遗忘）抗 off-beat 误检，与 ECG Pan-Tompkins 同构。
- **采样率**：约 46.3 kHz（板上实测校准）；支持原始波形 + BPM 曲线实时双显示，静音自动归零。

## 关键成果

- 打通 codec / I2C / I2S / DMA 全链路，自定义 HLS IP（bpm_detect）上板实测。
- 系统性解决 AXI-DMA 收帧死锁（DMA 块长与 FIFO 深度匹配 LEN=512 + 超时取数）。
- 对真实音乐节奏识别稳定，节拍器强脉冲信号仍在持续迭代优化。

## 目录结构

- `hardware/hls` —— bpm_detect HLS IP 源码与 C 仿真（bpm_detect.cpp/.h、tb.cpp）
- `hardware/tcl` —— Vivado Block Design 集成脚本（design_1_BYPASS.tcl）
- `notebooks/` —— PYNQ / Jupyter 实时采集与双波形可视化
- `report/` —— 作品报告（docx / pdf）
- `docs/` —— 排障记录、链路打通笔记与硬件诊断报告
- `assets/` —— 运行截图 / 波形图

## 运行环境

- Xilinx PYNQ-Z2
- Vivado 2024 / Vitis HLS 2024.2
- PYNQ overlay + Python 3
