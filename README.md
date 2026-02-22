# ESP32 OLED Anime Animation + MAX7219 8x32 Clock (NTP)

This project runs **two displays simultaneously** on an ESP32:

- **SSD1306 128x64 OLED (U8g2, I2C)**  
  Plays a custom anime-style animation sequence (Sharingan → Rasengan → Konoha → One Piece → Going Merry → Zoro Z cuts → Aura → Reform back to eye).  
  Also prints a **yellow-zone title**: `Uzumaki D Scar`.

- **MAX7219 8x32 LED Matrix (4× 8×8 chained)**  
  Displays a clean digital clock:
  - Format: `H:MM AM` or `HH:MM PM` (no leading zero for hour)
  - Blinking colon
  - Seconds progress bar on the bottom row
  - Orientation support via flip options

---

## ✨ Features

### OLED (SSD1306)
- Eye frame with rotating tomoe states  
- Particle break transition  
- Rasengan + compression effect  
- Konoha symbol draw + ignite + flame ring  
- Jolly Roger build + dissolve  
- Ocean wave animation  
- Going Merry sail-in + hold  
- Zoro Z cuts + aura charge + Asura flash  
- Reform back into eye (loop animation)  
- Centered top-band title text  

### MAX7219 8x32 Matrix
- Live NTP-based time (WiFi)
- 12-hour clock with AM/PM
- Clean 1-pixel spacing layout
- Blinking colon every second
- Seconds progress bar (bottom row)
- Vertical and horizontal flip support

---

## 🛠 Hardware Required

- ESP32 development board  
- SSD1306 OLED 128×64 (I2C, typically address `0x3C`)  
- MAX7219 8×32 LED matrix (4 modules chained, FC-16 type recommended)  
- Jumper wires  
- Common ground between modules  

---

## 🔌 Wiring

### OLED (I2C)
| OLED Pin | ESP32 Pin |  
|----------|-----------|  
| VCC      | 3.3V / 5V (dep ends on module) |  
| GND      | GND |  
| SDA      | GPIO 21 |  
| SCL      | GPIO 22 |  

### MAX7219 8x32
| MAX7219 Pin | ESP32 Pin |  
|-------------|-----------|  
| VCC         | 5V |  
| GND         | GND (common) |  
| DIN         | GPIO 23 |  
| CLK         | GPIO 18 |  
| CS          | GPIO 5 |  

---

## 📚 Required Libraries

Install via Arduino Library Manager:

- U8g2
- MD_MAX72XX
- SPI (built-in)
- WiFi (ESP32 built-in)
- time.h (ESP32 built-in)

---

## 🌐 WiFi & NTP Time

The matrix clock syncs using NTP servers:

- pool.ntp.org
- time.google.com
- time.nist.gov

Timezone is configured for **India (+05:30)**.

If WiFi or NTP fails, the matrix falls back to a millis()-based seconds animation.

---

## ⚙ Configuration

### Matrix Orientation
If your matrix appears upside-down or mirrored:

    static const bool MX_FLIP_Y = true;   // vertical flip
    static const bool MX_FLIP_X = false;  // horizontal mirror

### Matrix Brightness

Currently set to lowest intensity:

    mx.control(MD_MAX72XX::INTENSITY, 0);

You can increase it from 0–15 if needed.

---

## 🎬 Output

- OLED: Continuous anime animation loop with top-band title
- MAX7219: Live clock with seconds progress bar

---

## ⚠ Notes

- Ensure common ground between ESP32, OLED, and MAX7219.
- MAX7219 works best with stable 5V supply.
- Some OLED modules accept 5V; others require 3.3V — check your module.

---

## 👤 Author

Made by Uzumaki D Scar  
ESP32 + U8g2 + MD_MAX72XX
