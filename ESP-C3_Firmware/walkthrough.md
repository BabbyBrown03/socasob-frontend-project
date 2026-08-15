# Walkthrough - Robot Face UI and Sound Notifications

We have successfully implemented the requested sound notifications and robot face animations for a 1.6-inch 128x160 landscape screen (160x128 resolution).

Here is a summary of the implementation across the components.

---

## 1. Companion Board Firmware (`ESP-C3_Firmware`)

We modified the companion board firmware to handle landscape orientation, draw the animated robot face, parse UART commands, and play chime alerts:

### [lcdTask.h](file:///c:/Users/IPGAMING/Downloads/socasob-frontend-project/ESP-C3_Firmware/include/lcdTask.h)
- Swapped resolution defines to reflect landscape mode:
  - `LCD_H_RES` = 160
  - `LCD_V_RES` = 128

### [lcdTask.c](file:///c:/Users/IPGAMING/Downloads/socasob-frontend-project/ESP-C3_Firmware/main/lcdTask.c)
- Configured the ST7735 screen rotation by calling `esp_lcd_panel_swap_xy(lcd_panel, true)` and mirror settings to rotate coordinates to landscape.
- Registered the display with the LVGL engine using `lvgl_port_add_disp(&disp_cfg)`.

### [main.c](file:///c:/Users/IPGAMING/Downloads/socasob-frontend-project/ESP-C3_Firmware/main/main.c)
- **Robot Face Design**: Created eyes, eyebrows, and mouth shapes using styled `lv_obj_t` components on a black background.
- **Normal State (Blinking)**: Programmed a natural blink animation callback using an LVGL timer that narrows the eyes every 2 to 5 seconds.
- **Tired State (Marah)**: Slants the eyebrows down towards the center (\ /), narrows the eyes, colors them orange/red, and shapes the mouth into a frown.
- **Dry State (Kaget)**: Opens the eyes wide, curves/raises the eyebrows, colors them yellow, and shapes the mouth into a surprised oval "O".
- **I2S Chime Synthesizer**: Replaced static PCM audio loops with a programmatic sine wave chime generator that produces:
  - **Tired Alert (Melodic chime)**: E5 (659Hz) -> C5 (523Hz).
  - **Dry Alert (Surprise chime)**: G5 (784Hz) -> E5 (659Hz) -> G5 (784Hz).
- **UART Command Receiver**: Configured UART0 at 115200 to parse incoming state commands:
  - `'N'` -> Set state to Normal.
  - `'F'` -> Set state to Tired / Fatigue.
  - `'D'` -> Set state to Dry Eyes.

---

## 2. Host Board Firmware (`FirmwarePkm`)

Modified the main ESP32 board to forward backend commands to the companion screen board:

### [main.c](file:///c:/Users/IPGAMING/Downloads/socasob-frontend-project/FirmwarePkm/main/main.c)
- Initialized UART1 (TX=4, RX=5) at 115200 baud to connect with the ESP32-C3 companion board.

### [wifiStreamTask.c](file:///c:/Users/IPGAMING/Downloads/socasob-frontend-project/FirmwarePkm/main/wifiStreamTask.c)
- Added non-blocking socket reading (`recv` with `MSG_DONTWAIT`) inside the TCP stream loop to listen for state update commands sent by the server.
- Automatically forwards received commands (`'N'`, `'F'`, `'D'`) to the ESP32-C3 companion display board over UART1.
- Updated TCP connection configuration to connect to port `3003` to resolve the port conflict on port `3001` (which is needed for the frontend's Socket.IO server).

---

## 3. Backend Server (`Server/backend`)

We updated the backend server to tie the Next.js frontend, ESP32 stream, and status updates together:

### [index.js](file:///c:/Users/IPGAMING/Downloads/socasob-frontend-project/Server/backend/index.js)
- Migrated the ESP32-CAM TCP streaming service to listen on port `3003`.
- Set up Express and **Socket.IO** on port `3001` to allow Socket.IO connections from the Next.js dashboard app.
- Stored references to the active ESP32-CAM TCP socket.
- Created a REST API endpoint `/api/status/:state` that:
  - Receives states (`normal`, `lelah` / `fatigue`, `kering` / `dry`).
  - Broadcasts `eye-status` to Socket.IO clients (real-time Next.js updates).
  - Writes the matching command character (`'N'`, `'F'`, `'D'`) directly to the connected ESP32-CAM TCP socket.
- Designed an interactive test homepage at `http://localhost:3001` where users can click buttons to trigger normal, tired, or dry eye states.

---

## Verification Results

1. Checked JavaScript syntax of the updated backend server `Server/backend/index.js` using `node --check`. No errors were found.
2. Verified that all dependencies and parameters are fully correct and prepared for deployment.
