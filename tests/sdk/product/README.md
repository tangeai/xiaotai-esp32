# 生成产品主机回归

用生产产品清单、选型生成器和板包链接一份主机程序，验证模块、应用和主题组合能从 `xiaotai_start()` 走到实际的 `system.started`。有 PCM 输入和输出的产品还要证明媒体层找得到采集前端。物理零件和外部服务用本地夹具，不连接设备、路由器或云端。

在 Linux 或 WSL 的仓库根目录运行，需要 Python 3.10+、GCC/G++、CMake 和 CTest 3.21+。彩屏场景使用仓库锁定的本机 LVGL 源码；缺少组件会报错，不会替换成另一份或联网下载。

```bash
# 改过公共应用、选型或板包时，运行所有默认产品场景。
python3 -B tests/sdk/product/run.py -j 12 --parallel-products 3

# 对实际清单能独立删去的可选模块及命名变体，也重新生成、链接、启动。
python3 -B tests/sdk/product/run.py --optional -j 12 --parallel-products 3

# 只看一款板的启动问题。此命令只出局部结果，不能用来签整机复验。
python3 -B tests/sdk/product/run.py --product xiaotai-waveshare-p4-rev3 --boot-only
```

| 场景 | 实际核对 |
| --- | --- |
| 五款 SDK 产品 | 保存过凭据与空凭据两种启动，核对板包能力、配网入口和实际媒体能力 |
| 仲裁 | 真实的活动表和框架资源抢占；覆盖有方向的规则，不只读配置字段 |
| 两款 P4 | 从本地假网卡的真实请求获得地址，经原存储 API 比较 394 字节的旧历史格式 |
| WT99 | 门禁默认前台，伴侣按事件接管和归还；分别从已绑定和未绑定状态起步 |
| Xmini | 热点、已绑定待机和 AI 会话阶段的内存申请台账，按产品上限判定 |
| 启用 A-law 固定帧的 Xmini | `ai_fixed_rx` 使用真实 Transport 和 Media，验证残帧合并、PTS 回绕、100 ms 预填、200 ms 满环和订阅闸门；不替代 C3 内存台账或实板收包 |
| Xmini 单色屏 AI | `ai_mono_elapsed` 检查两次会话的页头计时与按键退场；模拟当前 Mini 的带代次关闭和退役查询，验证资源未释放时保持信令暂停、释放后恢复 |

默认给每次运行新建编译目录，退出时只清理本次新建的目录；系统强制终止时可能来不及清理。可用 `TMPDIR` 指定临时目录的上级，例如空间足够时用 `TMPDIR=/dev/shm`。日志和 CTest 已生成的 JUnit 单独保存在 `~/.local/state/xiaotai/product-host/evidence-*`，设置了绝对路径的 `XDG_STATE_HOME` 时以它代替 `~/.local/state`。也可用 `--evidence-root` 指定上级；实际证据路径由脚本打印。

传入 `--build-root` 可保留并复用同一版本的缓存；未指定 `--evidence-root` 时，证据也继续放在该目录下。容量或启动条件不合格时保留失败日志并返回非零；若配置或编译已失败，CTest 不会生成 JUnit。`--boot-only`、单产品结果、或只通过旧的板包单测，都不能替代完整产品验证。

主机记录的是任务栈请求值和本仓端口的内存申请，并在台账中注明原生配置与外部库的边界。它不提供 ESP32 真机剩余堆、实板音画、信号强弱或固件烧录结论。
