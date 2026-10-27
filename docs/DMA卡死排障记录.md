# DMA 卡死排障记录（XSA36 新IP）

> 2026-10-19 实测定案。核心：**DMA 收帧卡死 = segment IP 每帧丢最后 1 个 burst，wait/idle 永不返回。**

## 症状
- `dma.recvchannel.wait()` / `idle` 轮询**永久卡死**，cell 无输出。
- 或 `RuntimeError: DMA channel not idle`（上一帧没跑完就开下一帧，连锁）。

## 关键铁证（帧长 vs 实际收到）
| 写入 N | 实际收到 | 差值 |
|--------|----------|------|
| 32768  | 32736    | **32 字** |
| 8192   | 8160     | **32 字** |

**恒少 32 字 = 1 个 AXI burst（128 字节）**。规律性极强 → 是 hardware 端固定边界行为，非软件问题。

## 根因（读 segment_stream RTL 确认）
链路：`adau → axis_data_fifo_0 → segment_stream → bpm_detect → axi_dma`
- segment_stream 顶层是透传字数计数门：`m_axis_tvalid = s_axis_tvalid && (count!=0)`，`count` 每通过一字减 1，值来自软件写 REG0。
- 但 M_AXIS 模板带 **`C_M_START_COUNT = 32`**（design_1.tcl 第 292 行）初始握手延迟/内部缓冲。
- 结果：最后一个 burst 的 valid 在 tlast 断言时与 DMA 的 BTT 字节计数**末端时序不对齐**，最后 32 字被 DMA 地址计数器落在 BTT 之外 → 丢弃 → DMA 永远等不满 BTT → idle 永不拉高 → wait/轮询死锁。
- 这是 **segment IP 的 start-count 与 DMA BTT 末端对齐问题，硬件特性，软件改不了**。

## 解决方案（成功绕开，不再卡）
1. **DMA `start()` 只在初始化调用一次**；每帧 `flush → seg.write(REG0,N) → transfer → wait/invalidate`。**严禁每帧重复 `start()`**（第二次必报 `DMA channel not idle` 或拿旧数据）。
2. **不要 `wait()`/等 idle**——它永远等不来。DMA 收帧改用**超时轮询**（等 ~1.5s 直接取数据），收到 ~8160/8192 就够画图，末尾少 32 字肉眼无感。
3. **BPM 与 DMA 完全解耦**：BPM 由 PL freerun 计算，直接读寄存器就有值，根本不依赖 DMA 收没收满。
   - XSA36 新IP 基址 0x40 → **BPM@0x58、峰数@0x5C、包络@0x60**。

## 判据模板（probe_dma 用）
- 某 transfer 超时（returned=False）→ 源端没喂满 N（segment/fifo）。
- 全部帧逐元素相同但返回快 → 读到旧缓冲（cache/segment 重触发）。
- 帧不同且返回快 → **DMA 通路 OK**，剩余是音/增益/阈值问题。

## 经验结论
- 卡在 DMA wait 时先用"小帧 vs 大帧收到字数对比"定位是否丢 burst。
- 丢固定 burst → 是 source 末端对齐，绕开（超时轮询 + BPM 独立读寄存器），不要纠结那 32 字。
- 后续增益过高（swing~1.5M）还会引发检测器阈值自锁（BPM=0 peaks=0），需降 0x400E/F 增益。

## 相关文件
- 最终双图：`goal_dual_nodma_idle.ipynb`（DMA 超时轮询 + BPM 读寄存器 + G1波形 + G2曲线）
- 降增益探针：`lowgain_bpm_probe.ipynb`
- 源码：`_ip/segment/segment_stream_1.0/hdl/segment_stream_v1_0.v`
