---
status: active
audience: public
last-verified: 2026-07-26
---

# Getting Started on Windows

Zanna Tools supports Windows 10 version 1809 or newer and Windows 11 on x64
and ARM64. The installer is self-contained: it does not run PowerShell,
download packages, or require a separately installed Microsoft C++
redistributable.

## Install Zanna Tools

Run the installer that matches the computer's architecture. A signed release
should show its expected publisher in Windows. Do not bypass an unexpected or
invalid signature warning.

The first page offers four clear paths:

- **Install recommended** installs the compiler tools, Zanna Studio, SDK files, and
  available integrations.
- **Install SDK tools** installs the command-line tools and native development
  files without the IDE-focused extras.
- **Install everything** also installs all packaged samples.
- **Customize** lets you choose user or machine scope, a Unicode-capable custom
  folder, individual components, PATH, safe file associations, and shortcuts.

Current-user setup is the default and needs no administrator approval. A
stable release defaults to `%LocalAppData%\Programs\Zanna`; an all-users install
defaults to `%ProgramFiles%\Zanna`. Local builds use a separate development
identity and `Zanna development` directory so they can coexist with a stable
release.

The source-file integration adds Zanna to **Open with** for `.zia`, `.bas`, and
`.il`. It does not take over an existing default application. PATH and shortcut
entries are explicitly owned by the selected Zanna package and are removed
only if that package created them.

Open a new terminal after setup so it receives the updated PATH. You can also
open **Zanna Developer Prompt** from the Start menu. That prompt sets
`ZANNA_HOME`, `Zanna_DIR`, and `CMAKE_PREFIX_PATH` for installed SDK use.

## Verify the Installation

In a new PowerShell window, run:

```powershell
zanna --version
zanna eval '40 + 2' --type
```

If PATH integration was not selected, launch the Zanna Developer Prompt or run
`bin\zanna.exe` from the chosen installation folder.

## Your First Program

Create `hello.zia`:

```zia
module Hello;

bind Zanna.Terminal;

func start() {
    Say("Hello, World!");
}
```

Run it:

```powershell
zanna run .\hello.zia
```

The expected output is `Hello, World!`.

## Installed Developer Features

The recommended setup includes the Zanna command-line tools, language servers,
Zanna Studio, headers, static libraries, CMake package files, documentation, and
available editor integration. An external CMake project can use the installed
SDK from the Zanna Developer Prompt:

```cmake
find_package(Zanna CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE zanna::il_core zanna::il_io)
```

Git, CMake, Ninja, Visual Studio C++, the Windows SDK, VS Code, and Windows
Terminal are optional companion tools; setup detects but never downloads or
changes them. Running Zia, BASIC, IL, Zanna Studio, and the VM needs none of those
tools. Native code generation and compiling an external C++ consumer require
an architecture-matched compiler, linker, and Windows SDK.

## Change, Repair, or Remove Zanna

Open **Settings > Apps > Installed apps**, find the exact Zanna channel, and
choose its maintenance action:

- **Modify** changes components and integrations.
- **Repair** verifies installed hashes and restores missing or modified owned
  files while preserving unrelated developer files.
- **Uninstall** removes only files, PATH entries, registry values, associations,
  and shortcuts owned by that package.

Running the installed `uninstall.exe` directly also starts the verified cached
maintenance package, so the installation directory can be removed completely.
If a Zanna process has files open, setup identifies it with Windows Restart
Manager and offers a safe close-and-retry path.

Installation and maintenance are transactional. A power loss or terminated
process leaves a journal that the next setup or maintenance run recovers before
making another change. A newer installed version is protected from accidental
downgrade.

## Unattended Setup

Use `/quiet` for no interface or `/passive` for progress without prompts. Quote
paths containing spaces.

```powershell
.\zanna-toolchain-windows-x64.exe /install /quiet /norestart `
  /scope user /type typical /log "$env:TEMP\zanna-setup.log"

.\zanna-toolchain-windows-x64.exe /install /quiet /norestart `
  /installDir 'D:\Developer Tools\Zanna' /type sdk /noAssociations
```

Maintenance accepts `/modify`, `/repair`, or `/uninstall`. Other automation
switches include `/components <comma-separated-ids>`, `/addToPath`, `/noPath`,
`/associations`, `/noAssociations`, `/shortcuts`, `/noShortcuts`,
`/closeApplications`, and `/allowDowngrade`. Run `setup.exe /?` for the exact
contract embedded in that package.

Package inspection and update discovery are non-mutating automation operations.
Use `/output <path>` for reliable capture from services, build systems, and
other hosts that do not attach standard output to graphical applications. The
installer replaces that UTF-8 JSON file atomically or returns an error; it never
reports success after producing a partial or missing document.

```powershell
./zanna-toolchain-windows-x64.exe /inspect /quiet /output package.json
./zanna-toolchain-windows-x64.exe /checkForUpdates /quiet /output update.json
```

Important exit codes are:

| Code | Meaning |
|---:|---|
| 0 | Success |
| 87 | Invalid command line or path |
| 1602 | User cancellation |
| 1603 | Fatal setup error; inspect the log |
| 1618 | Another Zanna lifecycle operation is active |
| 1638 | A newer version is already installed |
| 3010 | Success; restart is required |

Without `/log`, setup writes a redacted UTF-8 log named
`ZannaInstaller-<package-id>-<UTC-time>-<pid>.log` under `%TEMP%`.

## Build Zanna and the Installer from Source

Source builds require CMake 3.20 or newer and Visual Studio or Build Tools with
the **Desktop development with C++** workload and a Windows SDK. For ARM64,
install the ARM64 C++ build tools and ARM64 MSVC libraries as well. Open a
Developer PowerShell for Visual Studio in an existing Zanna source checkout:

```powershell
.\scripts\build_zanna_win.ps1
```

The canonical script builds and tests Zanna with MSVC by default. Optional
fast-iteration settings include:

```powershell
$env:ZANNA_SKIP_CLEAN = '1'
$env:ZANNA_CMAKE_GENERATOR = 'Ninja'
$env:ZANNA_BUILD_TYPE = 'Release'
.\scripts\build_zanna_win.ps1
```

Build a local developer installer with:

```powershell
.\scripts\build_installer.ps1 --target windows
```

The interactive installer uses Zanna Games' dark charcoal, green, steel, and
teal visual language from its first screen through completion. It still uses
native controls for keyboard and screen-reader access, switches to Windows
system colors in high-contrast mode, and scrolls on small or highly scaled
work areas.

Debug CRT payloads are rejected by default. Use Release or RelWithDebInfo for a
package intended for another developer. The wrapper enables Zanna Studio when
the setting is absent and verifies both `zannastudio.exe` and
`zannastudio.buildinfo` before packaging. That metadata binds the exact Zanna
version, PE32+ architecture, byte size, and SHA-256; packaging rejects a
partial, stale, or mismatched Studio pair. Supplying `--stage-dir`,
`--build-dir`, or `--verify-only` reuses caller-owned input and skips the
automatic build. See the
[installer release guide](../installer-release.md) for signing, reproducible
release metadata, recursive verification, update manifests, and the complete
validation matrix.

## Troubleshooting

### `zanna` is not recognized

Open a new terminal, use the Zanna Developer Prompt, or modify the installation
and enable PATH integration. Existing terminals retain their old environment.

### Setup reports another operation is active

Wait for the other Zanna setup, repair, or uninstall process to finish and run
setup again. Lifecycle operations are serialized to prevent two packages from
editing the same owned state concurrently.

### Setup reports files in use

Save work and close the listed Zanna applications, then retry. In unattended
deployment, `/closeApplications` asks Restart Manager to close eligible
applications; `/norestart` prevents automatic restart behavior.

### Setup returns 1638

The installed package is newer. Obtain a newer installer, uninstall that
channel first, or use `/allowDowngrade` only when the rollback is intentional.

### Native code generation cannot find a linker or SDK

Install the architecture-matched Visual Studio C++ build tools and Windows SDK,
then use a Developer PowerShell or the Zanna Developer Prompt. The Zanna runtime
itself remains installed and usable without those optional native build tools.

### Source build cannot find a C++ compiler

Modify Visual Studio or Build Tools and select **Desktop development with C++**.
Then open a fresh Developer PowerShell and rerun the canonical build script.

## What to Read Next

- [Zia tutorial](../tutorials/zia-tutorial.md)
- [Zia reference](../languages/zia-reference.md)
- [BASIC tutorial](../tutorials/basic-tutorial.md)
- [General getting started guide](../getting-started.md)
