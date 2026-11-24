# Procedural expression source

The XiaoTai project owner confirmed internal use and public distribution
of the existing procedural expressions with the XiaoTai SDK and firmware.
The permission covers this expression model's poses, colors, and motion
tracks. It does not extend to unrelated images, fonts, or marks.

Original source in the XiaoTai development repository: Git commit
`3140fcfea002e3481809646bcf43d83b46b37898`,
`main/xiaotai/ui/s3/starter_product/src/starter_product_s3_face.inc`, SHA-256
`f97dfc6218f2cb5a337c9386e507ec62d69fe8cbee0b85ea106b4ff2bc1eb829`.
The same expression model is built once by the LCD kit.

`phone_image.inc` is the call-screen phone image of the same legacy S3
product, moved here unchanged from
`main/xiaotai/ui/s3/starter_product/src/starter_product_s3_phone.inc`
(commit `0f93f4147b2978211f47ce5e80c5739449451892`), SHA-256
`0dc7807052028e9861c646e20bc6024eb166e540b4069fca3612ec185543f948`. This renderer
does not compile the original expression file or any test art.
