# Companion WeChat Code

`wechat_code.c` contains the same WeChat mini-program code image shipped
by the legacy S3, P4, and WT99 companion UI. The project owner confirmed
this specific image for internal and public distribution with XiaoTai.
That confirmation does not extend to other images, fonts, or marks.

Source: `main/xiaotai/ui/{s3,p4,wt99}/starter_product/src/starter_product_wechat_qr.inc`
at commit `0f93f4147b2978211f47ce5e80c5739449451892`; one copy is kept as
`test/legacy_wechat_qr.inc`. All three source copies had SHA-256
`fd3e69e498d8df6546b73f264e6eaae62fd0657310439de4d468470f37d0d9f1`.
The conversion removes the 8-byte LVGL I1 palette and inverts each
remaining byte without changing row stride, bit order, or visible pixels.
The resulting 4,440 MONO1 pixel bytes have SHA-256
`3adea0dddb741ea43cff3188c8045f95209f14ddc37201c8992adb1a923a1e66`.
