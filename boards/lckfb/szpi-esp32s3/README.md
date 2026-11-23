# 立创实战派 ESP32-S3 V1.0.1

板包对应 `xiaotai-szpi-s3` 产品。这里保留板上有的零件、布线和内存配置；AI、通话、界面和联网功能由产品组合选定。

| 外设 | 布线和要点 |
| --- | --- |
| I2C0 | GPIO1（SDA）/ GPIO2（SCL），100 kHz；触摸、编解码器和相机 SCCB 共用 |
| PCA9557 | `0x19`；IO0 屏片选低有效、IO1 功放高有效、IO2 相机电源低有效 |
| ST7789 + FT5x06 | 320×240，SPI3：GPIO40（MOSI）、41（SCLK）、39（DC）；背光 GPIO42，低有效，只开关不调光 |
| ES7210 + ES8311 | I2S0：GPIO38（MCLK）、14（BCLK）、13（WS）、12（DIN）、45（DOUT）；16 kHz，本地采集四槽、输出双槽 |
| GC2145 | DVP，GPIO5 输出 20 MHz XCLK；相机需要时才上电 |
| 用户键 | GPIO0，上拉低有效，50 ms 消抖；启动时已按住不产生按键事件 |

配置面向 16 MiB Flash、80 MHz 八线 PSRAM 的板。分区表和旧工程一致，包含 `nvs`、`phy_init`、`factory`，没有 OTA 分区。

在 Linux 或 WSL 的仓库根目录运行板级主机检查：

```sh
python3 -B tests/sdk/run.py --board lckfb/szpi-esp32s3 --skip-dependency-tests -j 2
```

该检查运行正式板类及共享零件代码，核对描述符、上电顺序和出错回收。外设由主机夹具模拟；屏幕、收音、扬声器、摄像头和联网效果还要在真机上逐项验收。
