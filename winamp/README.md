# Winamp plugin: Stereo Switch

`gen_stereo.dll` is a general purpose plugin for Winamp 5.x (built for 5.666).
It adds an equalizer-sized skinned window with two buttons:

| Button | Request | Receiver input |
|---|---|---|
| Computer | `POST http://<host>/api/send?hex=c05004` | MD/DAT |
| TV | `POST http://<host>/api/send?hex=c05016` | TV |

While the window is open it polls `GET /api/state` every 5 s. The light on a
button is lit when the receiver is on that input. The bottom line is blank on
Computer or TV. Otherwise it names the other input (e.g. "Tuner") or a problem
("Receiver is off", "Can't reach ...").

The window opens at the classic equalizer size (275×116), which is also its
maximum, and can be shrunk to 138×78 by dragging its bottom-right
corner. As it shrinks, the labels keep their size and
the status line is dropped. With a modern skin, the skin decides how the window resizes.

## Install

1. Close Winamp and copy `gen_stereo.dll` into `C:\Program Files (x86)\Winamp\Plugins\`.
2. Start Winamp. Toggle the window with **Stereo Switch** in the main menu
   (right-click the main window, under Equalizer). It docks like the EQ.
3. To change the bridge address, open **Options > Preferences > Plug-ins > General Purpose**,
   select *Stereo Switch*, click **Configure**. The default is `slink.local`.
   `host:port` and plain IPs work too. **Test** checks that the bridge answers.

Settings are stored in `winamp.ini` under `[gen_stereo]`.

## Build (Linux)

```
sudo apt install gcc-mingw-w64-i686
make
```
