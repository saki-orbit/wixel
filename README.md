# Wixel（舱知 CatCabin）

**校园智能 IoT 硬件，让静音舱状态在线可见。**

静音舱不能预约，学生常常要跑到舱门口才知道里面有没有人。Wixel 用毫米波雷达检测舱内状态，通过手机网页显示“有人”或“空闲”，减少白跑；设备上的 OLED 也会随状态切换像素表情。

## 用到的技术

ESP32-C3、LD2410C 毫米波雷达、Arduino C++、Wi-Fi、HTTPS/JSON、Adafruit IO、SH1106 OLED（I²C）和 PLA 3D 打印外壳。设备不使用摄像头或麦克风。

## 目录

- `firmware/CatCabin_OLED/`：雷达检测、云端上报与 OLED 表情联动的整机固件。
- `firmware/CatCabin_OLED_Demo/`：独立 OLED 表情演示程序。
- `firmware/diagnostics/`：Wi-Fi 扫描和连接诊断程序。
- `web/silent-booth-site/`：当前在线网站源码（ChatGPT Sites，第 3 版）。
- `web/miaoda-silent-booth-app/`：早期妙搭版手机网页源码。

## 烧录前配置

将对应目录中的 `secrets.example.h` 复制为 `secrets.h`，填写自己的 Wi-Fi 名称、密码和 Adafruit IO Key。`secrets.h` 已加入忽略列表，不应提交到 GitHub。使用 Arduino IDE，选择 ESP32-C3 开发板；整机固件还需要安装 U8g2 库。

## 在线演示

[静音舱实时状态网页](https://library-silent-booth-status.z9myjwv29s.chatgpt.site/)

线上版网站从 Adafruit IO 读取设备上报，密钥由站点环境变量提供；妙搭版直接读取公开状态 Feed。演示原型使用 USB 供电，电池续航和多舱部署仍需进一步测试。
