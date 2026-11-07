# ETS2 Cabin Radio

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Windows%20x64-lightgrey.svg)](#installation--usage)
[![Release](https://img.shields.io/badge/Release-v1.2.0-brightgreen.svg)](https://github.com/uawannacry/ets2-spatial-radio/releases)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20Me-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/uawannacry)
[![Game](https://img.shields.io/badge/Euro%20Truck%20Simulator%202-Compatible-orange.svg)](https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/)

A native, zero-compromise real-time 3D spatial audio companion for **Euro Truck Simulator 2** on **macOS** and **Windows (64-bit)**. 

ETS2 Cabin Radio attaches directly to FMOD's `bus:/radio` channel inside the running game process to simulate an authentic 4.1-speaker truck/car cabin soundstage—**without modifying game executables, MP3 files, saves, game archives, or code signatures**.

---

## ✨ Features

- **4.1-Speaker Physical Cabin Acoustic Simulation**:
  - **Front Left (FL)**: Dash & door speaker positioned near the driver.
  - **Front Right (FR)**: Passenger-side dash speaker with realistic cross-cab Haas acoustic propagation delay.
  - **Rear Left (RL) & Rear Right (RR)**: B-pillar and sleeper berth surround speakers for full spatial immersion.
  - **Subwoofer Pressure**: Under-seat low-frequency resonance modeling.
  - **Early Reflections**: 4-tap reflection model simulating windshield, side glass, and rear sleeper back-wall acoustics.

- **Dynamic 360° Binaural Spatialization**:
  - **Fractional-Sample ITD (Interaural Time Difference)**: Accurately shifts sound arrival between ears as you pan your view.
  - **2-Band Head Shadowing**: Low-frequency diffraction vs. high-frequency occlusion when looking away from dashboard speakers.
  - **Front/Back Pinna Directivity**: Seamlessly responds to 360° interior cabin camera rotation (`cabin_rot`).

- **Real-Time Glass & Window Aerodynamics**:
  - Dynamically responds to left and right electric windows (`wnd_left`, `wnd_right`).
  - Models realistic acoustic pressure drop and exterior environmental wind bleed when opening windows or leaning out (`cabin_out`).

- **Live Hot-Reloading**:
  - Switch between presets (**Premium**, **Warm**, **Stock Radio**, **Spatial**) and adjust intensity, sleeper balance, or dashboard position on the fly—even while driving at 90 km/h.

- **Zero-Footprint Non-Destructive Hooking**:
  - Leaves game files, audio files, Steam achievements, and telemetry untouched.
  - Leaves game engine sounds, turbo spool, tire noise, and ambient world audio buses completely unaffected.

- **External Audio Bridge** (NEW):
  - Play audio from external apps (Chrome, YouTube, Spotify) through the in-game truck radio with full 3D cabin processing.
  - An external producer writes audio samples to a shared memory ring buffer; the DLL reads and plays them on `bus:/radio`.
  - See `cabin_bridge.h` for the IPC contract and `tools/bridge_test.c` for a test tone producer.

---

## 🎛️ Sound Presets

| Preset | Description |
| :--- | :--- |
| **Premium** | High-fidelity sound system with clear treble, tight low-end, and subtle studio cabin reflections. |
| **Warm** | Smooth analog curve with rich midrange and rounded low frequencies, optimized for long nighttime highway drives. |
| **Stock Radio** | Factory dashboard paper-cone speakers with authentic midrange coloration and gentle compression. |
| **Spatial** | Maximized stereo width and enhanced early cabin reflections for maximum 3D acoustic immersion. |

---

## 🚀 Installation & Usage

### 🍏 macOS (Apple Silicon / Intel)

1. Download **`ETS2-Cabin-Radio-v1.2.0-macOS.zip`** from [Releases](https://github.com/uawannacry/ets2-spatial-radio/releases).
2. Unzip and drag **`ETS2 Cabin Radio.app`** to your `/Applications` folder.
3. Open **`ETS2 Cabin Radio.app`** and click **Launch ETS2 with Live DSP**.
4. Adjust sound profile, cabin intensity, and driver position at any time in the companion window.
5. Diagnostic logs are written in real time to:
   ```text
   ~/Library/Logs/ETS2 Cabin Radio/live-dsp.log
   ```

---

### 🪟 Windows (64-bit)

1. Download **`ETS2-Cabin-Radio-v1.2.0-Windows-x64.zip`** from [Releases](https://github.com/uawannacry/ets2-spatial-radio/releases).
2. Extract the folder. Keep `ETS2 Cabin Radio.exe` and `ETS2CabinDSP.dll` together in the same directory.
3. Run **`ETS2 Cabin Radio.exe`**.

You have two launch options:

- **Option A: Portable Live Attach (Zero files in game folder)**:
  - Click **Launch / Attach Live DSP**.
  - Works both to launch ETS2 directly, or to attach live into an already running `eurotrucks2.exe` session without restarting the game.
- **Option B: Steam Auto-Plugin**:
  - Click **Install Steam Auto-Plugin** inside the launcher (or copy `ETS2CabinDSP.dll` to your `<Steam>\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\` folder).
  - ETS2 will automatically load the live cabin DSP whenever you launch the game from Steam or desktop shortcuts.

Diagnostics and runtime configurations are stored at:
```text
%LOCALAPPDATA%\ETS2 Cabin Radio\live-dsp.log
```

---

## 🛠️ Building from Source

### Prerequisites

- **macOS**: Xcode Command Line Tools (`xcode-select --install`).
- **Windows / Cross-compiling**: [Zig](https://ziglang.org/) compiler (for cross-compiling Windows x64 binaries from macOS/Linux), or MSVC (Visual Studio / Build Tools).

### macOS Build

Run the automated build script:
```bash
chmod +x build-macos.sh
./build-macos.sh
```

Or run manual commands:
```bash
# 1. Compile SwiftUI frontend launcher
swiftc -parse-as-library -swift-version 5 \
  -sdk "$(xcrun --show-sdk-path)" \
  -target arm64-apple-macos13.0 \
  -framework SwiftUI -framework AppKit \
  -o ETS2CabinRadio main.swift

# 2. Compile Live DSP injection library
clang -dynamiclib -O2 -Wall -Wextra -arch x86_64 -mmacosx-version-min=10.13 \
  -Wl,-undefined,dynamic_lookup -install_name '@rpath/libETS2CabinDSP.dylib' \
  -o libETS2CabinDSP.dylib LiveCabinDSP.c
```

### Windows x64 Build

Run the automated cross-compile script:
```bash
chmod +x build-windows.sh
./build-windows.sh
```

Or run manual commands:
```bash
# 1. Compile Windows 64-bit Live DSP DLL
zig cc -target x86_64-windows-gnu -shared -O2 -Wall -Wextra \
  -o ETS2CabinDSP.dll LiveCabinDSP.c

# 2. Compile Windows 64-bit GUI Launcher
zig cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Wl,--subsystem,windows \
  -o "ETS2 Cabin Radio.exe" windows_main.c app.res \
  -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -luser32 -ladvapi32
```

---

## 📁 Repository Structure

```text
ets2-spatial-radio/
├── LiveCabinDSP.c       # Core 4.1-cabin 3D audio DSP & FMOD real-time hooking engine
├── cabin_bridge.h       # IPC contract for external audio (shared memory ring buffer)
├── windows_main.c       # Native Windows Win32 companion launcher GUI & injection loader
├── main.swift           # Native macOS SwiftUI companion launcher & live manager
├── Info.plist           # macOS application bundle manifest
├── app.ico              # Windows application icon
├── app.res              # Windows compiled resource file (icons, metadata)
├── AppIcon.png          # macOS application icon asset
├── build-macos.sh       # macOS automated build and signing script
├── build-windows.sh     # Windows cross-compilation build script
├── tools/
│   └── bridge_test.c    # Test tone producer (440 Hz) for validating the audio bridge
├── HALLAZGOS.md         # Ghidra analysis of ETS2 FMOD internals (Spanish)
├── PLAN_AUDIO_EXTERNO.md# Architecture plan for external audio bridge (Spanish)
├── RELEASE_NOTES.md     # Release notes for version 1.2.0
├── LICENSE              # GNU General Public License v3.0 (GPL-3.0)
└── README.md            # Project documentation
```

---

## 📜 License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

### Why GPL-3.0?
- **Protection Against Commercial Reselling & Code Theft**: No third party can take this code, make it proprietary, hide the source, and sell it.
- **Open Source Forever**: Any derivative work, fork, or modification must also be distributed with full source code under the GPL-3.0.
- **Forks and Contributions Welcome**: Community forks, improvements, and pull requests are warmly encouraged!

See the [LICENSE](LICENSE) file for complete details.

---

## 🤝 Contributing

Contributions, feature suggestions, and bug reports are welcome!
1. Fork the repository.
2. Create your feature branch (`git checkout -b feature/awesome-spatial-tweak`).
3. Commit your changes (`git commit -m 'Add new acoustic reflection filter'`).
4. Push to the branch (`git push origin feature/awesome-spatial-tweak`).
5. Open a Pull Request.

---

## ☕ Support

If you enjoy using **ETS2 Cabin Radio** and want to support its development, maintenance, and future updates, consider buying me a coffee:

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/uawannacry)

Direct link: [ko-fi.com/uawannacry](https://ko-fi.com/uawannacry)

---

## ⚖️ Disclaimer

*Euro Truck Simulator 2 is a registered trademark of SCS Software. This project is an independent open-source audio companion tool and is not affiliated with, endorsed by, or sponsored by SCS Software or Firelight Technologies (FMOD).*
