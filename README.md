# winemenu

**winemenu_bridge** is an experimental Proof-of-Concept (PoC) tool designed to bridge the gap between Windows applications running under **Wine** and native Linux file managers.

## 🎯 Project Goal
The primary objective is to dynamically discover and extract the exact Command-Line Interface (CLI) commands hidden behind Windows right-click context menus (e.g., "Extract Here" in WinRAR or 7-Zip). It exports these actions into a structured JSON format, allowing Linux file managers (like Dolphin, Nautilus, or Thunar) to natively replicate the Windows context menu experience.

## 🚀 How It Works
The tool relies on advanced reverse engineering techniques written in C++:
1. **COM Menu Discovery:** Initializes `IContextMenu` via COM and populates a dummy `HMENU`. This safely extracts the actual UI text (bypassing owner-drawn menu limitations and avoiding useless `STRINGTABLE` garbage).
2. **Dynamic Command Interception (API Hooking):** Uses inline API Detours on `ShellExecuteExW` within the current process. When the tool virtually "clicks" a menu item, the hook intercepts the underlying raw CLI command, records it, and gracefully blocks the actual execution.
3. **Smart Grouping:** Consolidates extensions with identical context menu behaviors (e.g., `.rar`, `.zip`, `.7z`) to output a clean, highly optimized JSON file.

## 🚧 Limitations & Challenges
Pushing Wine to its limits revealed several hard roadblocks that prevented a 100% universal solution:
* **Architecture Mismatches (32-bit vs. 64-bit):** Calling `CoCreateInstance` on certain InProcServer32 DLLs fails under mixed Wine prefixes (yielding `0x80040154 Class Not Registered`).
* **Memory Fragility:** Injecting absolute JMP hooks into core Windows APIs (like `kernel32.dll`'s `CreateProcess`) inside Wine frequently results in unhandled page faults and process crashes. Hooking was strictly limited to `shell32.dll` for stability.

## 💌 A Message to the WineHQ Team
This project demonstrates what is possible but also highlights the limitations of an external bridging tool. **We strongly hope the Wine development team adopts this concept natively.** By implementing built-in `IContextMenu` translation and command bridging inside Wine itself, Linux users could finally enjoy seamless, native file manager integration with their installed Windows applications.

## 🛠️ Compilation & Usage
Compiled via `MinGW-w64` on Linux:

```bash
x86_64-w64-mingw32-g++ -static winemenu_bridge.cpp -o winemenu_bridge.exe -lole32 -luuid -lshell32
```

**Usage:**

```bash
wine winemenu_bridge.exe WinRAR --verbose --output results.json
```

---

*Developed with passion for reverse engineering and the open-source Linux community.*
