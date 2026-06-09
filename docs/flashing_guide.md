# 🔌 ESP32 WiFi Extender — Flashing Guide

Step-by-step instructions to install and flash the firmware onto your ESP32 board.

---

## Prerequisites

- **ESP32 board** (SY-111, DevKit V1, or any WROOM-32 based board)
- **USB cable** (Micro-USB or USB-C, depending on your board)
- **Computer** running Windows, macOS, or Linux
- **VS Code** installed ([download](https://code.visualstudio.com/))

---

## Step 1: Install PlatformIO

1. Open **VS Code**
2. Click the **Extensions** icon in the left sidebar (or press `Ctrl+Shift+X`)
3. In the search bar, type: **PlatformIO IDE**
4. Click **Install** on the "PlatformIO IDE" extension by PlatformIO
5. Wait for the installation to complete (it downloads the C++ toolchain, which can take 5-10 minutes)
6. **Restart VS Code** when prompted

You should now see a small **PlatformIO** icon (alien head) in the left sidebar and new buttons in the bottom toolbar.

---

## Step 2: Install USB Drivers

Most ESP32 boards use either a **CP210x** or **CH340** USB-to-serial chip.

### Windows
- **CP210x**: Download from [Silicon Labs](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- **CH340**: Download from [WCH](http://www.wch-ic.com/downloads/CH341SER_EXE.html)

### macOS
- Usually auto-detected. If not, install [CP210x for macOS](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)

### Linux
- Usually built into the kernel. Run: `sudo usermod -a -G dialout $USER` then log out/back in.

> **Tip:** Plug in your ESP32 and check Device Manager (Windows) or `ls /dev/tty*` (Linux/macOS) to confirm it's recognized.

---

## Step 3: Open the Project

1. In VS Code, go to **File → Open Folder**
2. Navigate to the `esp32-wifi-extender` folder and click **Open**
3. PlatformIO will automatically:
   - Detect the `platformio.ini` configuration
   - Download the ESP32 Arduino framework (first time only, ~500MB)
   - Download required libraries (ESPAsyncWebServer, AsyncTCP)
4. Wait until the bottom status bar shows **"PlatformIO: Ready"**

---

## Step 4: Build the Firmware

1. Look at the **bottom toolbar** in VS Code — you'll see PlatformIO buttons
2. Click the **✓ (checkmark)** button to **Build**
3. The output terminal will show compilation progress
4. Wait for: `SUCCESS` message

```
========================= [SUCCESS] ============================
Environment    Status    Duration
-------------  --------  ----------
esp32dev       SUCCESS   00:00:XX
========================= [SUCCESS] ============================
```

> **If build fails:** Make sure PlatformIO has finished downloading all dependencies. Check the error messages for missing libraries.

---

## Step 5: Upload to ESP32

1. **Connect** your ESP32 to your computer via USB
2. Click the **→ (arrow)** button in the PlatformIO toolbar to **Upload**
3. PlatformIO will automatically detect the COM port and flash the firmware

```
Writing at 0x00010000... (100%)
Wrote 786432 bytes in XX seconds
Leaving...
Hard resetting via RTS pin...
========================= [SUCCESS] ============================
```

### If Upload Fails

Some ESP32 boards require you to manually enter bootloader mode:

1. **Hold down** the **BOOT** button on the ESP32
2. While holding BOOT, **press and release** the **EN/RST** button
3. **Release** the BOOT button
4. **Immediately click Upload** in VS Code

If it still fails:
- Try a different USB cable (some cables are charge-only, no data)
- Try a different USB port
- Check that the correct COM port is selected in PlatformIO

---

## Step 6: Open Serial Monitor

1. Click the **🔌 (plug)** button in the PlatformIO toolbar to open the **Serial Monitor**
2. Make sure the baud rate is set to **115200**
3. You should see the boot messages:

```
╔══════════════════════════════════════════════╗
║                                              ║
║        ESP32 WiFi Extender v1.0.0           ║
║        ─────────────────────────             ║
║   Dual-Mode WiFi Repeater with NAPT         ║
║                                              ║
╚══════════════════════════════════════════════╝

Chip: ESP32-D0WDQ6  Rev: 1
CPU:  240 MHz  Cores: 2
Flash: 4096 KB  Heap: 298 KB

── Initializing WiFi ──────────────────────────
[WiFi] Initializing dual-mode WiFi (AP + STA)...
[WiFi] ✓ Access Point started!
[WiFi]   AP SSID    : ESP32_Extender
[WiFi]   AP IP      : 192.168.4.1
```

---

## Step 7: First-Time Configuration

1. On your **phone or laptop**, open WiFi settings
2. Find and connect to: **`ESP32_Extender`**
3. Enter password: **`ext12345678`**
4. Open a web browser and go to: **`http://192.168.4.1`**
5. The browser will prompt you for credentials. Enter:
   - **Username:** `admin`
   - **Password:** `ext12345678` (default password)
6. Once logged in, the premium iOS Light Mode Liquid Glass dashboard will load!
7. Click the **"WiFi Setup"** tab
8. Click **"Scan"** to find available networks
9. Click on your home WiFi network
10. Enter the password and click **"Connect"**
11. The serial monitor will show connection progress
12. Once connected, the ESP32 will start routing internet traffic ✅

---

## Updating Firmware

To update the firmware later:

1. Make your code changes
2. Click **Build** (✓) to verify no errors
3. Click **Upload** (→) to flash the new version
4. Your saved WiFi settings will be **preserved** (they're in NVS flash, separate from firmware)

---

## Factory Reset

If you need to start fresh:

### Via Web Portal
1. Connect to the ESP32 hotspot
2. Open `http://192.168.4.1`
3. Go to **"System"** tab
4. Click **"Factory Reset"**

### Via Serial
1. Open Serial Monitor
2. The ESP32 will restart with factory defaults

### Via Re-Flashing
1. Change settings in `src/config.h`
2. Rebuild and upload — NVS data persists unless you explicitly erase the flash:
   ```
   pio run --target erase
   pio run --target upload
   ```

---

## Common Issues

| Issue | Solution |
|-------|----------|
| **"No module named 'serial'"** | Install: `pip install pyserial` |
| **COM port not detected** | Install USB drivers (Step 2) |
| **Permission denied (Linux)** | Run: `sudo usermod -a -G dialout $USER` |
| **Upload stuck at "Connecting..."** | Hold BOOT + press EN/RST (Step 5) |
| **Build error: lwip/lwip_napt.h** | Make sure you're using PlatformIO, not Arduino IDE |
| **Unauthorized / 401 Error on Web Portal** | Log in with username `admin` and your current AP password (default `ext12345678`). Perform serial factory reset if password is forgotten. |
| **Device blocked or disconnected** | Check the **Blacklist** tab inside the web portal. Remove the device's MAC if it was blacklisted. |

---

## Need Help?

- Check the **serial monitor** output for error messages
- Open an issue on the project repository
- Join the ESP32 community forums at [esp32.com](https://esp32.com)
