# 模块怎么测传输

只想测模块自己的状态机时，可以用 `INCLUDE_ONLY xiaotai_transport`，在测试里实现它实际调用的几个 `link.h` 函数。要测连接代次、收发门、回调顺序或连接释放，就链接 `xiaotai_transport`。主机构建会自动选脚本对端，不会加载 TiRTC 静态库，也不会联网。

## 先跑一个例子

在仓库根目录运行：

```sh
python3 -B tests/sdk/run.py --skip-dependency-tests -j 3 xiaotai_transport
```

从 [wp6_test.c](wp6_test.c) 的 `link_results()` 看一条最小的外连：准备虚构账号，把传输引擎启动到在线，让模块申请连接，注入连接结果，最后确认关闭。想看打开收发门之后怎么喂音频，参考 [wp3_test.c](wp3_test.c) 的 `test_audio_profiles()`。公共夹具在 [support.h](support.h) 和 [fakes.c](fakes.c)，分别负责所有者任务同步，以及身份、平台、网络和媒体的假实现。

| 想检查 | 下一步 |
| --- | --- |
| 连接成功、迟到或失败 | 在所有者任务调用 `xiaotai_link_connect`，用当次代次注入 `xiaotai_transport_host_connected`。连接句柄用不同的非零测试值即可。要模拟同步回调，用 `xiaotai_transport_host_connect_hook`。 |
| 对端订阅与本机许可 | 用 `xiaotai_transport_host_peer_subscribe` 模拟对端；用 `xiaotai_link_permit` 模拟业务握手。两者不要都开，不然断言分不出是谁开了发送门。 |
| 拥塞或底层拒绝 | 用 `xiaotai_transport_host_backlog_set` 设所有连接的默认积压，用 `xiaotai_transport_host_backlog_for` 单独控制某个句柄；用 `xiaotai_transport_host_media_fail` 让指定后端调用失败。 |
| 回调交错 | 用测试信号控制交错；媒体调用钩子不持有脚本对端的锁，可以在其间注入帧或关闭。避免靠随手加一段 sleep 猜线程进度。 |
| 超时和重试 | 开机前设手动时钟；先推进到截止前一刻并断言没有动作，再推进到截止，核对状态和 `xiaotai_transport_host_last_at`。后端只保留前 256 条记录，但总次数和最近时刻继续更新。 |

脚本事件由调用它的测试线程注入；模块回调通常要等框架所有者任务消费。检查结果前让工作任务和所有者任务都排空，参考夹具的 `settle()`。`xiaotai_transport_host_settle()` 本身只等工作任务。钩子上下文和借用的连接参数遵从各自生命周期，释放后不要继续读。保持每个夹具只使用合成数据。

这套用例能检查应用调用次序、业务门控和边界条件。它不能证明交付库的实际行为、弱网音质、FreeRTOS 调度、板上栈水位或堆碎片；这些留给对应台架检查。
