# Kylio Robot Car — ESP-IDF

Native ESP-IDF firmware for the Kylio robot car. One L298N controls the two
left motors together on channel A and the two right motors together on channel
B. The web dashboard has basic directional controls and a landscape HUD with
an analog joystick for variable speed and curved turns. Telegram sends one
startup notification with the web address.

## Wiring

See [HOW_TO_DRAW_WIRING_DIAGRAM.md](HOW_TO_DRAW_WIRING_DIAGRAM.md) for the
current motor and ESP32 wiring checklist. GPIO18, GPIO5, GPIO26, GPIO25,
GPIO33, and GPIO32 are no longer used by this firmware.

## Configure local secrets

Create the local configuration file once:

```powershell
Copy-Item .\main\secrets.example.h .\main\secrets.local.h
```

Edit `main/secrets.local.h` and set:

- the Wi-Fi SSID and password;
- a newly generated Telegram bot token and its allowed chat ID;
- a strong username/password for the web dashboard.

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
dashboard address, for example `http://192.168.1.42`. The browser will request
the username and password configured in `secrets.local.h`.

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
available viewport. Keep the L298N ENA and ENB jumpers enabled; PWM uses
IN1–IN4, so the existing wiring needs no additional GPIO connections.

## Safety behavior

- All motor GPIOs are driven LOW during startup.
- Both control tabs send a heartbeat every 100 ms while held; the firmware
  stops the motors if that heartbeat disappears for 350 ms. Releasing the
  joystick or pressing Stop cuts motor power immediately.
- Long-press text selection and context menus are disabled over the controls.
  If a native selection or drag begins anyway, the page releases the active
  control and sends Stop.
