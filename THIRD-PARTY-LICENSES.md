# Third-Party Components and Licenses

FA2Entropy is licensed under the **GNU Affero General Public License v3.0 or later**
(see [`LICENSE`](LICENSE)).

FA2Entropy is an **independent** program. It does not link against FA2sp, does not include any
FA2sp header or source file, and does not call into FA2sp. It is a separate DLL that is injected
into the map editor process by the same loader that FA2sp uses.

This file lists every third-party component involved and how it is used.

---

## 1. Components compiled into our binaries

| Component | License | How it is used |
|---|---|---|
| **MinHook** (Tsuda Kageyu) | BSD 2-Clause | Compiled into `FA2Entropy.dll`. Source is vendored in `third_party/minhook/` — upstream <https://github.com/TsudaKageyu/minhook>, commit `8af6b4ac`. License text: `third_party/minhook/LICENSE.txt`. |

The BSD 2-Clause license requires the copyright notice and the license text to be reproduced in
binary distributions. They are reproduced here, in `third_party/minhook/`, and in the release
package as `THIRD-PARTY.txt`.

---

## 2. Components distributed *alongside* the map editor (not part of this repository)

The map editor package ships the following third-party files next to `FA2Entropy.dll`.
They are **not** our code and are **not** covered by our license.

| Component | License | Upstream / source |
|---|---|---|
| **FA2sp** (FA2SP HDM Edition) | **AGPL-3.0** | https://github.com/handama/FA2sp |
| **Syringe / SyringeEx** (the injector that loads the DLLs) | **LGPL-3.0** (SyringeEx source repository) | https://github.com/handama/FA2sp (bundled binary) |
| Scintilla | HPND-style permissive | https://www.scintilla.org/ |
| Lexilla | HPND-style permissive | https://www.scintilla.org/Lexilla.html |
| Lua 5.4 | MIT | https://www.lua.org/license.html |
| OpenSSL (`libcrypto-1_1.dll`, `libssl-1_1.dll`) | Apache-2.0 | https://www.openssl.org/source/license.html |
| `CncVxlRenderText.dll` | distributed with the FA2sp HDM package | see the FA2sp distribution |
| `ddraw.dll` + `aqrit.cfg` | aqrit's DirectDraw compatibility wrapper | see the original distribution |
| `FinalAlert2YR.dat` and the Yuri's Revenge game assets | © Westwood Studios / Electronic Arts | This project claims **no** rights over them. |

If you redistribute the map editor package, keep the license files of these components with it.

---

## 3. Configuration data in this repository

`assets/Quantum01.ini`, `assets/Quantum02.ini` and `assets/FinalAlert.ini` are configuration data
that is compiled into `FA2Entropy.dll` (that is why `assets/` has to be part of the source release).

The copies in this repository hold **examples only**: they stand in for the editor's own
`FAData.ini`, `FALanguage.ini` and `FinalAlert.ini` and show the format and the keys that matter.
The data of a particular mod is not published here, so a build from this repository produces an
editor with placeholder content.

---

## 4. What this project deliberately does *not* do

* It does not link against, load, or call into FA2sp.
* It does not contain FA2sp headers or source code.
* It does not modify FA2sp or any other third-party binary. `FA2Entropy.dll` is a separate,
  independently injected DLL.
