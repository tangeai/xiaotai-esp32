# GUITION JC1060F570C（ESP32-S31，7 寸）

板包对应 `xiaotai-jc1060f570c` 产品。这里保留板上有的零件、布线和内存配置；AI、通话、界面和联网功能由产品组合选定。

| 外设 | 布线和要点 |
| --- | --- |
| 模组 | ESP32-S31-WROOM-3-N16R16V：16 MB Flash（QIO），16 MB 八线 PSRAM（200 MHz），CPU 320 MHz |
| I2C0 | GPIO2（SDA）/ GPIO3（SCL），400 kHz；触摸和编解码器共用 |
| 屏 | 7 寸 IPS 1024×600，16 位 RGB 并口：PCLK GPIO40（34 MHz，下降沿出数据）、HSYNC 44、VSYNC 45、DE 43；数据线 D0–D15 依次为 GPIO36、37、38、39、46、7、35、48、49、50、51、4、5、0、1、6；两块帧缓冲在 PSRAM 里扫描输出 |
| 背光 | GPIO52，20 kHz PWM，可调光 |
| GT911 | I2C 地址 `0x5D`；中断和复位没有接到芯片 |
| ES8389 | I2C 地址 `0x10`；I2S0：GPIO54（BCLK）、47（WS）、16（DIN）、56（DOUT），没有 MCLK，编解码器跟随位时钟；16 kHz，采集四槽（MIC1、回采、MIC2、空），输出双槽 |
| 功放 | NS4150，GPIO42 高电平开 |

板上还有 SD 卡（SDMMC 四线）、百兆网口（RMII）、RX8025T 时钟、RS485 和电池管理，这个板包暂不使用。

只支持 ESP-IDF 6.1 线。在 Linux 或 WSL 的仓库根目录运行板级主机检查：

```sh
python3 -B tests/sdk/run.py --board guition/jc1060f570c --skip-dependency-tests -j 2
```
