# 📡 ESP32 WiFi Extender

A powerful WiFi range extender built on the ESP32 (SY-111 / WROOM-32) that **simultaneously connects to your home WiFi** and **creates a hotspot** for distant devices. Internet traffic is routed via **NAPT (Network Address Port Translation)** — no additional hardware required.

---

## ✨ Features

| Feature | Description |
|---------|-------------|
| **Dual-Mode WiFi** | Simultaneous Station (STA) + Access Point (AP) operation |
| **NAPT Routing** | Full network address translation — AP clients get internet through STA |
| **Web Dashboard** | Premium iOS Light Mode Liquid Glass theme (soft morphing background, glassmorphism, mouse-glow tracking, Inter font) |
| **Curved Traffic Graph** | Live download/upload bandwidth monitoring with smooth cubic bezier curve splines |
| **Security Lock** | HTTP Basic Authentication protecting all server endpoints |
| **Network Scanner** | Scan and select available WiFi networks from the browser |
| **Auto-Reconnect** | Automatically reconnects if home WiFi drops |
| **Persistent Config** | Settings saved to NVS flash (including blacklist/credentials) — survives reboots |
| **Status LED** | Visual connection status indicator |
| **Client Management** | View connected devices, hostnames, MACs, active speeds, and enforce dual-layer blocking |
| **Factory Reset** | One-click reset to clear all settings |

---

## 🔧 Hardware Requirements

| Component | Details |
|-----------|---------|
| **ESP32 Board** | SY-111, DevKit V1, NodeMCU-32S, or any ESP32-WROOM-32 board |
| **USB Cable** | Micro-USB or USB-C (depends on your board) |
| **Power Supply** | USB power (5V) — can use a phone charger for standalone operation |

> **No additional components needed!** The ESP32's built-in WiFi radio handles everything.

---

## 💻 Software Requirements

1. **[VS Code](https://code.visualstudio.com/)** — Free code editor
2. **[PlatformIO IDE](https://platformio.org/install/ide?install=vscode)** — Extension for VS Code (handles ESP32 toolchain)

> ⚠️ **Why PlatformIO instead of Arduino IDE?**
> The standard Arduino IDE doesn't expose the NAPT (Network Address Port Translation) functions in the ESP32's lwIP stack. PlatformIO gives us access to the full ESP-IDF framework while keeping Arduino-style code.

---

## 🚀 Quick Start

### 1. Install PlatformIO

1. Open **VS Code**
2. Go to **Extensions** (Ctrl+Shift+X)
3. Search for **"PlatformIO IDE"**
4. Click **Install** and wait for it to finish
5. Restart VS Code when prompted

### 2. Open the Project

1. In VS Code, go to **File → Open Folder**
2. Navigate to and open the `esp32-wifi-extender` folder
3. PlatformIO will automatically detect the project and download required tools (this may take a few minutes on first run)

### 3. Connect Your ESP32

1. Plug your ESP32 into your computer via USB
2. Your OS should automatically install drivers
3. If not, install [CP210x drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) (for most ESP32 boards)

### 4. Build & Upload

1. Click the **✓ (Build)** button in the PlatformIO toolbar at the bottom of VS Code
2. Wait for compilation to complete (first build takes longer)
3. Click the **→ (Upload)** button to flash the firmware
4. Click the **🔌 (Serial Monitor)** button to see debug output

### 5. Configure Your Extender

1. On your phone or laptop, look for the WiFi network: **`ESP32_Extender`**
2. Connect with password: **`ext12345678`**
3. Open your browser and go to: **`http://192.168.4.1`**
4. When prompted by the browser for credentials, enter:
   - **Username:** `admin`
   - **Password:** `ext12345678` (or your custom AP password if you changed it)
5. Click the **"WiFi Setup"** tab
6. Click **"Scan"** to find your home WiFi network
7. Select your network and enter the password
8. The ESP32 will connect and start extending your WiFi! 🎉

---

## 📊 Web Dashboard

The configuration portal is designed with a premium **iOS Light Mode Liquid Glass theme** (featuring smooth morphing background gradients, frosted glass cards, Inter typography, and interactive mouse-glow spotlight tracking), and includes:

- **📊 Dashboard** — Live signal strength, uptime, memory, and **real-time curved speed graphs** (download/upload) updating every second.
- **📶 WiFi Setup** — Scan for networks, view signal strength, and connect with a password modal.
- **📡 Hotspot** — Change your extender's SSID and password.
- **👥 Clients** — Real-time connected device manager displaying Hostname, IP, MAC address, active speed, and **one-click blocking**.
- **🚫 Blacklist** — Dedicated list to manage and unblock blacklisted clients.
- **⚙️ System** — Device hardware details, manual reboot, and factory reset.

---

## 📶 Status LED Patterns

| Pattern | Meaning |
|---------|---------|
| 🔴 **Solid ON** | AP running, no home WiFi configured |
| 🟡 **Slow Blink** | Connecting to home WiFi... |
| 🟢 **Fast Blink** | Connected & routing — extender is active! |
| ⚫ **OFF** | Device is off or booting |

> The built-in LED is on **GPIO 2** for most ESP32 boards. If your board uses a different pin, change `STATUS_LED_PIN` in `src/config.h`.

---

## ⚙️ Configuration

Default settings can be changed in [`src/config.h`](src/config.h):

```cpp
// Hotspot defaults
#define DEFAULT_AP_SSID       "ESP32_Extender"
#define DEFAULT_AP_PASSWORD   "ext12345678"
#define DEFAULT_AP_MAX_CONN   8          // Max simultaneous clients

// NAPT performance
#define NAPT_TABLE_SIZE       512        // NAT translation table entries

// Status LED pin
#define STATUS_LED_PIN        2          // GPIO 2 (built-in LED)
```

> All settings can also be changed at runtime through the web portal. They persist across reboots.

---

## 📈 Performance

| Metric | Typical Value |
|--------|---------------|
| **Throughput** | 5–15 Mbps |
| **Max Clients** | 8 (configurable up to 10) |
| **WiFi Band** | 2.4 GHz only |
| **Range Extension** | 10–30 meters (depends on environment) |
| **Latency** | +2–5 ms overhead |

### Tips for Best Performance

1. **Placement**: Put the ESP32 **halfway** between your router and the dead zone
2. **Line of Sight**: Minimize walls between the ESP32 and your router
3. **Antenna**: If your board has an external antenna connector, use one
4. **Channel**: The AP automatically syncs to the STA channel for optimal performance
5. **Power**: Use a good quality USB power supply (unstable power = unstable WiFi)

---

## 🔍 Troubleshooting

| Problem | Solution |
|---------|----------|
| Can't find ESP32_Extender WiFi | Check power supply, try restarting the ESP32 |
| Can't open 192.168.4.1 | Make sure you're connected to ESP32_Extender, not your home WiFi |
| Unauthorized / Login Failed | Use username `admin` and the current AP password (default: `ext12345678`). If you changed the AP password and forgot it, perform a factory reset. |
| "Failed to connect" to home WiFi | Double-check password, ensure 2.4GHz network (ESP32 doesn't support 5GHz) |
| Connected but no internet | Wait 10 seconds for NAPT to initialize, check serial monitor for errors |
| Blocked device cannot reconnect | Ensure the client's MAC address has not been accidentally added to the **Blacklist** tab. Remove it to restore access. |
| Slow speeds | Normal — ESP32 is a microcontroller, not a commercial router (5-15 Mbps expected) |
| Frequent disconnections | Improve placement, check power supply stability |
| Upload fails | Try holding the **BOOT** button on ESP32 while uploading, install CP210x drivers |

---

## 📁 Project Structure

```
esp32-wifi-extender/
├── platformio.ini              # Build configuration
├── src/
│   ├── main.c                  # Main firmware entry point
│   ├── config.h                # All configurable defaults
│   ├── wifi_manager.h/.c       # Dual-mode WiFi management
│   ├── napt_router.h/.c        # NAT routing engine
│   ├── web_portal.h/.c         # Web server & API routes
│   ├── index.html              # Frontend user interface dashboard
│   ├── index_html_gz.txt       # GZIP-compressed binary of index.html
│   └── status_monitor.h/.c     # Health monitoring & LED
├── README.md                   # This file
└── docs/
    └── flashing_guide.md       # Detailed flashing instructions
```

---

## 🔬 How It Works

```
┌──────────────┐       WiFi STA        ┌─────────────────┐
│  Home Router │◄──────────────────────►│                 │
│  (Internet)  │  192.168.1.x          │   ESP32 WiFi    │
└──────────────┘                        │   Extender      │
                                        │                 │
                         WiFi AP        │  NAPT Engine    │
┌──────────────┐◄──────────────────────►│  translates     │
│  Phone       │  192.168.4.x          │  packets between│
└──────────────┘                        │  AP ↔ STA       │
┌──────────────┐◄──────────────────────►│                 │
│  Laptop      │  192.168.4.x          └─────────────────┘
└──────────────┘
```

1. **STA Interface** connects to your home router and gets an IP (e.g., 192.168.1.100)
2. **AP Interface** broadcasts a hotspot with its own subnet (192.168.4.x)
3. **NAPT** translates packets: when a phone (192.168.4.2) requests google.com, NAPT rewrites the source to 192.168.1.100 before forwarding to the router, and rewrites responses back to 192.168.4.2

---

## 📄 License

MIT License — feel free to use, modify, and distribute.
