# E38 Holley EFI Gateway for Arduino

Extract the ZIP and open `E38_Holley_EFI_Gateway.ino` inside the same-named folder.
In Arduino IDE, select **AutosportLabs ESP-CAN-X2** from the Espressif Arduino
ESP32 boards package. Install Cory Fowler's [MCP_CAN library](https://github.com/coryjfowler/MCP_CAN_lib)
if it is missing. The sketch uses the library's SPI-class constructor and
three-argument receive overload. Open Serial Monitor at 115200 baud to see
controller initialization and transmit failure counts.

## Wi-Fi dashboard

Before uploading, set `DashboardPassword` in `Dashboard.h` to your own password
of at least 8 characters. The dashboard Wi-Fi stays disabled until a password is
configured. Connect your phone or laptop to **E38-Holley-Gateway** with that
password, then open **http://192.168.4.1** in its browser.
Keep the Wi-Fi connection if your device reports that this network has no
Internet access. The Serial Monitor prints the dashboard address.
Change the SSID and password constants at the top of `Dashboard.h` before
uploading if desired. WiFi and WebServer come with the Arduino ESP32 core.

The page refreshes about twice per second and displays all five GM-to-Holley
parameters, Holley park and gear inputs, and the GM gear output. Rows include
CAN IDs, data bytes, update counts, age, and the most recent transmit result.
Park and gear input IDs are displayed as masked groups because the gateway
accepts the low 11 bits from any matching Holley sender.
Values are marked stale after three seconds. This is a display threshold
independent of the Holley ECU timeout. A disconnected browser labels the
displayed values as frozen.

CAN2 statistics distinguish completed calls, pending-at-timeout returns,
unavailable TX buffers, and other errors. The display also includes CAN2 TEC
and EFLG plus CAN1 queue failures and missed receive frames. Counters are
cumulative since startup. CAN1 Queued reports acceptance into the TWAI queue.
CAN2 Completed reports the MCP library return value. Receiver acceptance
must be checked at the receiving ECU.

HTTP runs in a separate FreeRTOS task. It reads a synchronized snapshot.
The CAN loop owns all MCP2515 operations. The page is served entirely by the
board and can be used without an Internet connection.

This update is based on the previously delivered sketch. If you made additional
local edits, merge those into this version before uploading.

| Raspberry Pi | ESP-CAN-X2 | Bus | Bitrate | Pins |
|---|---|---|---|---|
| `can0` | CAN1 (TWAI) | GM HS-CAN | 500 kbps | RX GPIO6, TX GPIO7 |
| `can1` | CAN2 (MCP2515) | Holley EFI | 1 Mbps | HSPI CS GPIO10, SCK GPIO12, MOSI GPIO11, MISO GPIO13, 16 MHz MCP2515 clock |

The original Python script uses externally configured SocketCAN bitrates.
The sketch uses the settings from the existing ESP-CAN-X2 sketch. It processes
both directions in Arduino's `loop()`, which owns the MCP2515 SPI interface.
Each input triggers its corresponding output. Both DBCs were loaded by the
Python program, but its translations used explicit IDs and byte offsets.
Those translations are built into the sketch.

## CAN1 GM to CAN2 Holley

Incoming IDs are standard. Outgoing IDs are extended and carry one 32-bit
big-endian IEEE 754 float. Byte offsets are zero-based.

| GM ID | Signal | Holley ID | Conversion |
|---|---|---|---|
| `0x4D1` | Fuel level (%) | `0x1E202001` | Byte 5 × 0.392157 |
| `0x4D1` | Reduced power | `0x1E20E001` | Byte 3, bit 7 |
| `0x3D1` | A/C kick | `0x1E206001` | Byte 4, bit 5 |
| `0x3F9` | A/C high-side pressure (psi) | `0x1E20A001` | Byte 7 × 14 ÷ 6.895 |
| `0x3F1` | Outside air (°F) | `0x1E212001` | (Byte 7 × 0.5 − 40) × 1.8 + 32 |

## CAN2 Holley to CAN1 GM

CAN2 receives extended ID groups `0x1E52D000` (park) and `0x1E4FD000`
(gear), using mask `0x1FFFF800`. The first four bytes carry a big-endian
float. Park starts true and updates from the latest valid park frame. Each
valid gear frame triggers one standard GM `0x1F5` frame with DLC 8.

| Selected gear | Expected GM payload |
|---|---|
| Park (gear 0, park true) | `0F 0F 00 01 00 00 00 00` |
| Neutral (gear 0, park false) | `0D 0D 00 03 00 00 00 00` |
| Reverse (gear −1) | `0E 0E 00 02 00 00 02 00` |
| Forward gear 3 | `03 03 00 04 00 00 03 00` |

The port writes forward gears as numeric bytes. The Python helper returned
hex strings for those two bytes, which cannot be used directly as a
`python-can` payload.

## Bench check

With an ACK-capable CAN interface on each bus, send standard `0x3D1` with
payload `00 00 00 00 20` on CAN1. Expect extended `0x1E206001` with payload
`3F 80 00 00` on CAN2. Send extended `0x1E4FD001` with payload
`40 40 00 00` on CAN2. Expect the forward-gear 3 frame listed above on CAN1.

Build verification was attempted with Arduino ESP32 core 3.3.12. Windows
returned an Illegal System DLL Relocation error while launching the ESP32-S3
compiler. Arduino IDE Verify and hardware tests are still pending.

The Wi-Fi update's browser script passed checks for parameter rendering, pending
status, waiting for initial data, stale data, and loss of connection using
simulated data. A new build attempt reached the same compiler launch error.
Wi-Fi connections and CAN behavior still need testing on the physical board.
