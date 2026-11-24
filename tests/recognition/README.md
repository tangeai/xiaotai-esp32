# 声音识别 HTTP 专项

固件侧配置 API 和立创只读 AT 指令见 [声音识别](../../docs/功能/声音识别.md)。本目录的探针独立于固件，不把主机请求成功写成板上指令或录音播放已验收。

`probe.py` 用于获准的测试环境。它生成 1 秒 16 kHz 单声道 PCM16，不访问麦克风，也不读取现成音频文件。它不属于固件，不打开小钛上的录音功能。

**先查配置。** `--config-only` 只请求 `GET /v1/recognition/config`。它不用任务编号，不上传音频，也不要求识别已启用。输出仅含设备使用的策略字段、方案数和上传上限；配置有效但功能关闭也是一次成功的读取。

```sh
python3 tests/recognition/probe.py --config-only \
  --discovery-url http://platform.test.example/services
```

终端服务遵循项目的 [安全提示](../../README.md#安全提示)。发现请求不带凭证；发现结果提供有效的 HTTP `recognition-srv` 后，向该地址发送测试设备的 Bearer。也可用 `--endpoint http://recognition.test.example` 指定已确认的测试地址。脚本不猜测主机、不改写协议或端口，也不把 AI 对讲地址当识别服务。HTTP 无传输加密与服务器身份认证，只在已确认的测试环境使用，不输出凭据。

测试凭证经环境变量 `XIAOTAI_TEST_DEVICE_TOKEN` 提供，不放在命令参数、日志或 Git 中。提交专项还要求测试设备已绑定、识别生效并允许上传样例；服务可能计入识别额度，不使用生产设备。

```sh
python3 tests/recognition/probe.py \
  --endpoint http://recognition.test.example \
  --device-id fixture-s3 --request-id synthetic-001 --submit-synthetic
```

脚本仅使用 HTTP，拒绝重定向、URL 内凭据和非法地址，不再提供 `--ca-file`。提交回复丢失时用原 `request_id` 查任务，不自动重发录音；后端返回受理只算开始，最终 `succeeded` 才退出 0。其他终态或查询次数用尽退出 2，鉴权、协议或传输异常退出 1。手动复查时保留原编号，把 `--submit-synthetic` 换成 `--query-only`，不会上传音频或创建新任务。

协议参见 [ThingConnect 音频识别](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/api-reference.md#recognition-submit)。这个专项不验证 MQTT、模型准确性、板端采集、WAV 播报或抢占。
