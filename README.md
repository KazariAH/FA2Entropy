# FA2Entropy

A companion DLL for the **FinalAlert 2: Yuri's Revenge map editor** (FA2sp HDM Edition) that
adapts the editor to the **QEC (Quantum Entropy Change) mod**: interface language, game-resource
path and the mod's configuration files.

It is an independent DLL: the editor folder needs `FA2sp.dll` and `Syringe.exe`, which load it.

---

## What you have to change to run it

1. **Copy** `FA2Entropy.dll` and `FA2Entropy.exe` into your map-editor folder — the folder that
   contains `FA2sp.dll`, `Syringe.exe` and `FinalAlert2YR.dat`.

2. **Rename** `FinalAlert2YR.dat` to **`FA2Entropy.dat`**.
   The DLL does nothing while the editor is still called `FinalAlert2YR.dat`.
   If you start the editor with `RunFA2sp.bat`, change the file name inside it as well.

3. **Start** the editor with `FA2Entropy.exe` (or your updated `RunFA2sp.bat`).

That is all that is required.

> On the first start the DLL tidies the folder by itself: the editor's ini files move into
> `DataMain\` and the support DLLs into `DllMain\`. That is expected — leave those folders alone.

---

## Build

Visual Studio 2022 (v143, Win32) — open `FA2Entropy.sln` and build.

The configuration that gets compiled into the DLL lives in `assets\`.

---

## License

AGPL-3.0-or-later — see [`LICENSE`](LICENSE).
Third-party components and notices — see [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md).
