# plan

本文档是调参计划草案，不等同于当前代码默认值。

当前代码默认 EQ 频点（src/dspeq/play_eq_normal.c）为：

- 20 / 35 / 60 / 110 / 220 / 360 / 700 / 1.6k / 3.2k / 4.8k / 7.2k / 10k / 16k / 18k / 20k / 22k

当前模式边界（src/dspeq/play_dsp_common.h）为：

- Linear: <= 220Hz
- Dynamic: (220Hz, 16kHz]
- Normal: > 16kHz

## 01  EQ 可调整的分频点（计划项）
 
 50 / 80 / 160 / 300 高通, 去除低频杂音

 
 20 / 35 / 60 / 110 / 220 低频 eq 频点

 中频带通 360 / 700 / 1.6k / 3.2k / 4.8k / 7.2k / 10k / 16k

 18k 降 3db, 20k 降 24db