<!-- This README is translated by GitHub Copilot from the original Chinese README.md -->

# ESPaperPlay

English | [简体中文](README.md)

**ESP32-based e-ink display system with interactive UI**

ESPaperPlay is an e-ink display project based on **ESP32**, providing interactive interfaces for networked weather display, smart home control, system settings, and more. It uses a GDEY0154D67 1.54-inch e-ink screen driven by the **SSD1681** chip, and builds the UI with **LVGL v9**.

---

## 🌟 Features

- Real-time network information fetching
- Weather information display (temperature, humidity, and historical data)
- Smart home control (Home Assistant integration, not yet completed)
- Supports partial refresh to optimize e-ink display performance
- Extensible UI based on LVGL v9

---

## 🛠 Hardware Requirements

- ESP32-WROOM-32E
- GDEY0154D67 e-ink display (1.54 inch)

---

## 🚀 Installation & Usage

### 1. Clone the repository:
   ```
   git clone https://github.com/xiaobao/ESPaperPlay.git
   cd ESPaperPlay
   ```

### 2. Build and flash to ESP32 (ESP-IDF must be configured in advance):
   ```
   idf.py build
   idf.py flash
   idf.py monitor
   ```

### 3. First-time Wi-Fi setup
- Power on and connect your phone to a **2.4G Wi-Fi**;
- Open the **EspTouch** app and select *Esptouch mode* for network setup.

### 4. Running Effect
- The main e-ink screen will display **time information** and a **daily quote**;
- You can interact via the touchscreen.

### 5. Weather API Configuration
This project uses the **HeWeather API**. You need to set `api_key` and `api_host` on the device:

1. Click the corresponding setting entry in the device menu;
2. ESP32 will start a **TCP server**;
3. Use a network debugging tool:
   - Mode: TCP Client
   - IP address: ESP32's IP (viewable in settings)
   - Port: `8080`
4. After connecting, enter `api_key` or other config items in the send box;
5. Click **Save** on the device (some settings require a reboot to take effect).

> Note: Other configuration items are set in a similar way.

---

## 📄 License

This project is licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

---

## 🤝 Contributing

The project is still under development, with some bugs and unfinished features. You are welcome to submit Issues to help improve this project.
