# ESPaperPlay

**ESP32-based e-ink display system with interactive UI**  

ESPaperPlay 是一个基于 **ESP32** 的电子墨水屏项目，提供联网天气显示、智能家居控制、系统设置等的交互界面。使用 **SSD1681** 芯片驱动的GDEY0154D67 1.54 英寸墨水屏，并基于 **LVGL v9** 构建 UI。

---

## 🌟 特性

- 实时联网信息获取 
- 天气信息显示 （温度、湿度等历史数据）  
- 智能家居控制 （接入Home Assistant，尚未完成）
- 支持局部刷新，优化墨水屏显示性能  
- 可扩展的 UI 界面，基于 LVGL v9  

---

## 🛠 硬件需求

- ESP32-WROOM-32E  
- GDEY0154D67 电子墨水屏（1.54 英寸）  

---

## 🚀 安装与使用

1. 克隆仓库：
   ```
   git clone https://github.com/xiaobao/ESPaperPlay.git
   cd ESPaperPlay
   ```

2. 编译并烧录到 ESP32 （需要提前配置好ESP-IDF）：
   ```
   idf.py build
   idf.py flash
   idf.py monitor
   ```

运行后，墨水屏会显示时间信息和每日一言，可通过触摸屏进行交互。  

---

## 📄 License

本项目采用 **MIT License**，详见 [LICENSE](LICENSE) 文件。  

---

## 🤝 贡献

项目仍在开发中，存在一些BUG和部分未完成的功能；欢迎您提交Issue以帮助改进本项目。
