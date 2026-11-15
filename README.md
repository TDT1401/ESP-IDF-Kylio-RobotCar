# Kylio Robot Car — ESP-IDF

Native ESP-IDF firmware for the Kylio robot car. One L298N controls the two
left motors together on channel A and the two right motors together on channel
B. The web dashboard has basic directional controls and a landscape HUD with
an analog joystick for variable speed and curved turns. Telegram sends one
startup notification with the web address.

## Wiring

See [HOW_TO_DRAW_WIRING_DIAGRAM.md](HOW_TO_DRAW_WIRING_DIAGRAM.md) for the
current motor and ESP32 wiring checklist. Motor direction uses IN1=GPIO26,
IN2=GPIO27, IN3=GPIO12, IN4=GPIO13. PWM uses ENA=GPIO25 and ENB=GPIO14;
remove both enable jumpers when wiring these pins to the ESP32. GPIO18,
GPIO5, GPIO33, and GPIO32 are not used by this firmware.

## Configure local secrets

Create the local configuration file once:

```powershell
Copy-Item .\main\config\secrets.example.h .\main\config\secrets.local.h
```

Edit `main/config/secrets.local.h` and set:

- the Wi-Fi SSID and password;
- a newly generated Telegram bot token and its allowed chat ID.

`secrets.local.h` is ignored by Git. Do not reuse the Telegram token contained
in the imported Arduino archive; rotate it with BotFather first.

## One-time ESP-IDF tool setup

Run this once before the first build. It creates the ESP-IDF-specific Python
environment under `C:\Users\ASUS\.espressif` and downloads the required
Espressif tools. An Internet connection is required.

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' 'C:\esp\v6.1\esp-idf\tools\idf_tools.py' install
```

The commands below put the Espressif Python first for the current PowerShell
window. This avoids a `python` command supplied by pyenv being used by
`export.ps1`.

```powershell
$env:Path = 'C:\Espressif\tools\python\v6.1\venv\Scripts;' + $env:Path
. 'C:\esp\v6.1\esp-idf\export.ps1'
idf.py --version
```

## Build

From the project directory, initialize ESP-IDF as shown above, then run:

```powershell
idf.py set-target esp32
idf.py build
```

The first build may also download the managed `espressif/cjson` dependency.
Its resolved version is recorded in `dependencies.lock`.

## Flash and monitor

Replace `COM5` with the serial port used by the board:

```powershell
idf.py -p COM6 flash monitor
```

Exit the monitor with `Ctrl+]`. After the station connects, the log prints the
dashboard address, for example `http://192.168.1.42`. Open it directly in a
browser; the web dashboard has no login.

## Telegram notification

If a bot token and chat ID are configured, the car automatically sends one
notification after each boot when Wi-Fi connects. The message contains a link
to the web dashboard. Telegram does not receive commands or report status.
The dashboard shows motion, IP address, and Wi-Fi signal strength.
If Telegram still shows commands registered by an older firmware, remove that
saved command menu in BotFather. The new firmware ignores those commands.

## Web HUD

Open the **HUD** tab to enter the dark, gamepad-style full-screen controller.
Rotate a phone to landscape when prompted. The left side has the same four
directional buttons as the basic tab; the center shows left and right motor
output and the Stop button; the right side has the analog joystick. Drag the
joystick up or down to drive forward or backward; drag sideways while moving
to steer in an arc. The joystick mixes independent left and right motor power
from -100% to 100% and ramps changes smoothly. While moving, joystick commands
are sent at most every 50 ms; an unchanged hold sends a 100 ms heartbeat.
Wi-Fi modem sleep is disabled to reduce command latency, increasing ESP32 power
consumption. Touching either control takes over from the other. Use **Thoát
HUD** to return to the basic controls.
Releasing the joystick, leaving HUD, or losing command heartbeats stops both
sides. If the browser does not support the Fullscreen API, HUD still fills the
available viewport. Motor speed is controlled by PWM on the L298N ENA and ENB
pins; IN1–IN4 select direction.

## Bluetooth gamepad

The firmware scans for nearby Bluetooth HID devices, then lets you choose one
from the **Bluetooth** tab in the web dashboard. Set the controller's mode to
Bluetooth and put it in pairing mode before scanning. Select **Connect** beside
the controller. The same tab shows the connection state and has a disconnect
button; disconnecting stops the motors. No external Bluetooth UART module is
used.

The controller's left stick drives and steers the car when its HID report map
uses standard gamepad axes. A web control already being held keeps control
until it is released or times out. Re-scan to select another HID controller.
## Safety behavior

- All motor GPIOs are driven LOW during startup.
- Both control tabs send a heartbeat every 100 ms while held; the firmware
  stops the motors if that heartbeat disappears for 350 ms. Releasing the
  joystick or pressing Stop cuts motor power immediately.
- Long-press text selection and context menus are disabled over the controls.
  If a native selection or drag begins anyway, the page releases the active
  control and sends Stop.
