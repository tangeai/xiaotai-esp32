# WT SPI 诊断来源

记录日期：2026-11-03。

整机链接发现 `wifi_manager` 引用的 `eh_host_bus_spi_log_diagnostics` 来自原 WT 工程的本地 Hosted 派生源，并不在发布的 3.0.7 组件中。该缓存文件不属于最初的 4381 文件导入清单，因此本轮单独审查、补录来源，没有用空函数或删掉调用绕过。

- 原路径：`wt99p461-s1-xiaotai/managed_components/espressif__esp_hosted/host/mcu/eh_host_mcu_transport/src/eh_host_bus_spi.c`。
- 固定 3.0.7 输入 SHA-256：`772e8ec27103683c1f077583976c50a3a59d3bf12e7df8ce1b865804dbb54692`。
- 经审查的原 WT 派生源 SHA-256：`7b9e787418fbce6c8485069087573ddda11befdd48b7ca121d64e84bd8b1f4f4`。
- [板级适配](../../main/boards/wt99p461-s1/build-support/cmake/hosted_spi_diagnostics.cmake) 在构建目录生成该派生源，校验输入和输出哈希，再替换 Hosted 目标中的唯一 SPI 编译单元。共享依赖缓存不改写，原文件许可保留。

差异仅增加原有的传输/帧错误/空帧/超时 DR/STA 序号/耗时计数，由后台诊断请求输出；没有改变握手门控、TX 排队、DMA 释放、错误返回或帧分发。原注释中的 temporary 描述诊断用途，并非本轮网络故障的修复或规避。

对原 WT Hosted 目录逐文件比较还发现 RPC 文件仅换行不同，以及新缓存中已存在的微雪 SDIO bounce 适配；WT 选择 SPI，不进入 SDIO 路径。没有顺带合入其他并行工程变更。

[主机测试](../../tests/test_hosted_spi_diagnostics.py) 对实际原版和派生版 worker 分别执行普通与 ASan/UBSan 检查，覆盖 HS 未就绪、TX 释放、空帧/坏帧、传输失败、序号回绕/间断/重复/回退和 HCI 分发，核对数据路径相同及全部诊断字段。SPI 硬件时序和耗时测量仍待设备验收。
