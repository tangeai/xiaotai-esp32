<div align="center">

# XiaoTai ESP32

[简体中文](README.md) | **English**

**Real-time audio, video, and AI voice chat on ESP32**

[![MIT License](https://img.shields.io/badge/License-MIT-2EA043?style=flat-square)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/README.md)
[![ESP32-P4](https://img.shields.io/badge/ESP32-P4-E7352C?style=flat-square&logo=espressif&logoColor=white)](waveshare-esp32p4-xiaotai/README.md)
[![ESP-IDF 5.5.4](https://img.shields.io/badge/ESP--IDF-5.5.4-0969DA?style=flat-square)](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)
[![GitHub Stars](https://img.shields.io/github/stars/tangeai/xiaotai-esp32?style=flat-square&label=Stars&color=B8860B)](https://github.com/tangeai/xiaotai-esp32)
[![GitHub Issues](https://img.shields.io/github/issues/tangeai/xiaotai-esp32?style=flat-square&label=Issues&color=0969DA)](https://github.com/tangeai/xiaotai-esp32/issues)

[![TiRTC SDK](https://img.shields.io/badge/TiRTC-SDK-0969DA?style=flat-square)](lckfb-szpi-esp32s3-tirtc/third_party/tirtc/README.md)
[![WebRTC](https://img.shields.io/badge/WebRTC-333333?style=flat-square&logo=webrtc&logoColor=white)](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md)
[![LVGL](https://img.shields.io/badge/UI-LVGL-008FBE?style=flat-square)](lckfb-szpi-esp32s3-tirtc/dependencies.lock)
[![ESP-SR](https://img.shields.io/badge/Audio-ESP--SR-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md#音频链路)
[![MQTT](https://img.shields.io/badge/MQTT-660066?style=flat-square&logo=mqtt&logoColor=white)](lckfb-szpi-esp32s3-tirtc/components/platform_client/src/platform_client.c)
[![WeChat VoIP](https://img.shields.io/badge/WeChat-VoIP-07C160?style=flat-square&logo=wechat&logoColor=white)](#wechat-calls)

[Download firmware](#download-firmware) · [Browser flasher](https://espressif.github.io/esptool-js/) · [Demo platform](https://demo-open.tange-ai.com) · [Server & mini program](https://github.com/tangeai/tirtc-server-example)

</div>

XiaoTai uses TiRTC/WebRTC for live monitoring, WeChat calls, device-to-device calls, and AI voice chat. Download the firmware for your board, flash it, connect to Wi-Fi, and bind it to your account. No compilation or self-hosted server is required.

**Download BIN → Flash over USB → Connect to Wi-Fi → Bind on the website → Try the features**

<a id="download-firmware"></a>

## Step 1: Download the firmware

Prepare a supported board, a USB data cable, a computer, and a **2.4 GHz Wi-Fi** network with internet access. You will also need an email address to register on the platform.

| Board | Full BIN download (16 MB) | Features |
| --- | --- | --- |
| LCKFB Shizhanpai (立创·实战派) ESP32-S3 V1.0.1/N16R8 | [Download S3 1.0.0](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-s3-app-v1.0.0/xiaotai-esp32-s3-app-v1.0.0-full-16MB.bin) | Voice calls and AI voice chat; no camera video |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 | [Download P4 1.0.0](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.0.0/xiaotai-esp32-p4-app-v1.0.0-full-16MB.bin) | Audio/video calls, live video, and AI voice chat |

Check the chip revision before flashing: **S3 rev 0.0–0.99 or P4 rev 1.0–1.99**. P4 rev 2.x/3.x is not supported by these images. The chip revision is different from the PCB version. Do not flash S3 firmware onto P4, or vice versa.

These are **pre-release** images. See the [S3 release](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-s3-app-v1.0.0) or [P4 release](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.0.0) for flashing instructions, checksums, and validation status.

## Step 2: Flash from your browser

> Flashing a full BIN erases Wi-Fi credentials, binding information, and user settings stored on the device. Back up any settings you need first.

1. Connect the board using a USB data cable and close any software using its serial port. **For P4, use the main chip's flashing port. Do not flash the onboard C6.**
2. Open the [Espressif browser flasher](https://espressif.github.io/esptool-js/) in **Chrome/Edge** on your computer. Click **Connect** and select the board's serial port.
3. Click **Add File** and select the downloaded `*-full-16MB.bin`. Set **Flash Address to `0x0`**, and set Flash Mode, Flash Frequency, and Flash Size to **`keep`**.
4. Click **Program**. Once flashing succeeds, click **Disconnect** and press the board's reset button to restart it.

If no serial port appears, check that your USB cable supports data transfer. If connection fails, hold **BOOT**, press **RESET** once, release BOOT, and retry.

## Step 3: Connect to Wi-Fi

1. Follow the setup instructions on the device screen. S3 uses hotspot setup. P4 lets you select Wi-Fi on the screen or use hotspot setup.
2. For hotspot setup, connect your phone to the hotspot shown on the screen, then open the setup page. The S3 hotspot is named `XiaoTai-XXXX` and requires no password. If its setup page does not open automatically, visit `http://192.168.6.1`.
3. Select your **2.4 GHz Wi-Fi** network, enter its password, and submit. Once connected, the device displays a **6-digit binding code**.

Switch your phone or computer back to a network with internet access before opening the demo platform.

## Step 4: Bind the device on the website

The instructions below include the Chinese interface labels to help you find each control.

1. Open the [XiaoTai demo platform](https://demo-open.tange-ai.com), register, and sign in.
2. Go to [My Devices (我的设备)](https://demo-open.tange-ai.com/devices). Select **Add Device (添加设备) → Bind with Code (验证码绑定)**.
3. Enter the **6-digit code** shown on the device and click **Bind Device (绑定设备)**.
4. Return to the device list and check that the device shows **Online (在线)**. If the code has expired, follow the device prompts to get a new one.

## Step 5: Try the features

Live monitoring, WeChat calls, and AI voice chat each require one device. Device-to-device calls require two. Close the live view or end the current call before switching features.

### Live monitoring

1. In the website's device list, click **Live (实时)** for your device.
2. Click the sound button to hear audio from the device. P4 also provides live video when a camera is connected; S3 supports audio only.
3. Allow microphone access in your browser. Press and hold **Hold to Talk (按住说话)** to send your voice to the device.

### WeChat calls

1. Tap **WeChat Call (微信电话)** on the device. If there are no WeChat contacts, the screen displays a mini program QR code.
2. Scan it with WeChat and follow the mini program prompts to authorize voice/video calls from the device.
3. Sync the device's contacts, select an authorized WeChat contact, and start a call. Answer on your phone.

When WeChat contacts are already available, the **WeChat Call** shortcut calls the first WeChat contact in the list. S3 supports voice calls; P4 supports audio/video calls.

### Device-to-device calls

1. Set up the second device using steps 1–4. Devices bound to the same account automatically become contacts.
2. Sync contacts on the device. For devices on different accounts, add the other device through the website's **Contacts (联系人)** page and have the other user accept the request.
3. Select the other device and start a call, then answer on that device. Video calls are available when both devices support video.

### AI voice chat

1. End any active call and return to the device's home screen.
2. **Tap the face on the home screen**, or say **“你好小钛” (Nǐ hǎo Xiǎotài)**.
3. Once the device responds, start speaking. For example, say “介绍一下你自己” (“Introduce yourself”).

## Developer resources

The linked development guides are in Chinese.

- **Build and configuration:** [S3 guide](lckfb-szpi-esp32s3-tirtc/README.md) and [P4 guide](waveshare-esp32p4-xiaotai/README.md). Builds use ESP-IDF 5.5.4. Keep the full repository: P4 depends on the sibling S3 source and `common/` resources.
- **Architecture and code:** [S3 communication and audio](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md) and [P4 communication, audio, and video](waveshare-esp32p4-xiaotai/docs/P4_MEDIA_ARCHITECTURE.md).
- **Troubleshooting and limitations:** [S3 known issues](lckfb-szpi-esp32s3-tirtc/KNOWN_LIMITATIONS.md) and [P4 troubleshooting](waveshare-esp32p4-xiaotai/docs/TESTING.md).
- **Server, Web, and WeChat mini program:** [tirtc-server-example](https://github.com/tangeai/tirtc-server-example). For self-hosting, see the [deployment guide](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/deployment.md).

## Feedback and license

When [opening an issue](https://github.com/tangeai/xiaotai-esp32/issues), include your board model, firmware version, reproduction steps, and relevant logs. Remove passwords and keys from the logs before sharing.

This project uses the [MIT License](LICENSE). Third-party SDKs and assets retain [their respective licenses](THIRD_PARTY.md).
