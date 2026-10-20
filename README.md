# Kylio Robot Car — ESP-IDF

Native ESP-IDF firmware for the Kylio robot car. One L298N controls the two
left motors together on channel A and the two right motors together on channel
B. The web dashboard provides forward, backward, rotate-left, rotate-right,
and stop controls. Telegram sends one startup notification with the web address.

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

- one or more Wi-Fi SSID/password pairs;
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

## Safety behavior

- All motor GPIOs are driven LOW during startup.
- Manual controls send a heartbeat every 100 ms while held; the firmware stops
  the motors if that heartbeat disappears for 350 ms.
