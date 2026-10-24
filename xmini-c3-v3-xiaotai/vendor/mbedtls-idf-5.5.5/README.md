# Mbed TLS MPI 工作区生命周期补丁

来源为 [ESP-IDF v5.5.5](https://github.com/espressif/esp-idf/tree/v5.5.5/components/mbedtls)，Mbed TLS 子模块提交 `9d669eadb1955d348986b9280156710aaaadf79f`。本目录仅替换 `mbedcrypto` 中的 `bignum.c`，不修改安装的 ESP-IDF。

- 原始文件 SHA-256：`5decc03dc8fa4f9e476fa37e67c4186c63148ed70d72506282539a3966f86abf`
- 补丁文件 SHA-256：`45626c933a826d06b6a3782ccc252d6b10acd097c1be5fa3e27cd568dc4b49e4`
- [差异](bignum-workspace-lifetime.patch) 仅延后同容量 T 工作区的分配，避免与 R² 预计算、基数规范化临时内存重叠。分配失败仍返回原错误，清理和完整清零保持原样。

unsafe / secret 算法、窗口、位宽、硬件与大密钥软件 fallback、证书验证和安全选项均保留。CMake 锁定目标、版本、原始及补丁哈希和编译单元归属，SDK 更新后须重新审核，不能直接放宽校验。

保留上游版权和双许可证声明，完整许可证见 [LICENSE](LICENSE)。本工程按其中 Apache-2.0 选项使用。

原始和补丁实现已完成实际源码主机普通及 sanitizer 对照、故障注入和公开输入真机 KAT。组合录音缓存布局的单次正常压力对照未观测到分配失败；单独工作区或单独布局仍有失败记录。重复、长稳、相邻功能与完整产品回归尚未完成，不据此宣称全部内存问题已解决。
