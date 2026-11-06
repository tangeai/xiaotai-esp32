# 本地提示音

绑定播报由 `activation.opuspkt` 与六个数字片段组成，均为 16 kHz、单声道、20 ms Opus 包。固定引导语为“登录微信小程序搜索小钛进行绑定，钛是金属钛的钛”，随后播报六位验证码。文件只占用 Flash，播放时复用产品音频解码器，不常驻复制到 RAM。

数字提示音来自 `tbox-vb6824` 的 `main/assets`，按 MIT License 使用，版权声明为 `Copyright (c) 2025 yichone`。来电、拨号回铃、点击、成功、警告和挂断音均由 `tools/generate_interaction_sounds.py` 原创建模，再离线转换为独立的 Opus 资源；声音采用短促、清晰、层次明确的系统交互风格，不复制第三方系统铃声。固定中文引导语由 Windows 的 Microsoft Huihui Desktop 语音离线生成；对应 JSON 记录原文、声音名称、输入哈希与输出哈希。本目录中的 `.json` 文件记录源文件和输出文件的 SHA-256。资源由以下命令离线转换：

```text
python tools/prepare_opus.py SOURCE.p3 OUTPUT.opuspkt --p3-source --source-license MIT
```

| 资源 | 含义 | 播放时机 |
| --- | --- | --- |
| `wifi_provisioning.opuspkt` | “请打开 WiFi 来为设备配置无线网络” | `XiaoTai-XXXX` SoftAP 和配网页启动成功后播放一次 |
| `ring_incoming.opuspkt` | 明快双段来电铃声 | 收到来电后循环，接听、拒接或取消时立即停止 |
| `ring_outgoing.opuspkt` | 短促电话回铃音 | 设备发起呼叫后按“嘟、嘟、嘟”节奏循环，微信接听、挂断或失败时立即停止 |
| `call_ended.opuspkt` | 下降双音 | 正常挂断、拒接或取消后，回到空闲态时播放一次 |
| `popup.opuspkt` | 轻触反馈 | 普通本机交互，不承担通话状态语义 |
| `exclamation.opuspkt` | 警告反馈 | 启动、建连或通话超时等失败 |
| `success.opuspkt` | 完成确认 | 本机绑定身份清除完成，或连按四下清除 Wi-Fi 并准备进入配网 |

来电资源约 1.34 秒，附加 0.56 秒间隔，形成约 1.90 秒的清晰循环；拨号资源约 0.26 秒，附加 0.76 秒间隔，形成约 1.02 秒一次的短促回铃。循环与中断都由 `product_audio` 的既有解码任务负责，不新增任务或动态缓冲。通话媒体开始时会在 20 ms 包边界取消铃声，避免铃声混入通话下行；通话建立不额外插入提示音，以免遮挡首句语音和增加接通延迟。

短提示采用异步队列。BOOT 连按四下清除 Wi-Fi、保留本机身份；长按 7 秒清除本机身份、保留 Wi-Fi。账号与微信 VoIP 授权的云端解除仍由小程序完成。
