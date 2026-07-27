---
status: active
audience: public
last-verified: 2026-07-26
---

# Getting Started on macOS

This guide walks you through installing and running the Zanna compiler toolchain on macOS (Apple Silicon).

---

## Prerequisites

| Tool | Minimum Version | Purpose |
|------|-----------------|---------|
| Xcode Command Line Tools | latest | Provides Apple Clang (C++20), linker, and system frameworks |
| CMake | 3.20 | Build system generator |
| Git | any | Clone the repository |

### Install Xcode Command Line Tools

Open Terminal and run:

```bash
xcode-select --install
```

A dialog will appear — click **Install** and accept the license agreement. This provides Apple Clang (the canonical compiler for Zanna on macOS), Git, Make, and all required system frameworks (Cocoa, AudioToolbox, IOKit, CoreFoundation, ImageIO).

Verify the installation:

```bash
clang++ --version
```

You should see output like `Apple clang version 15.0.0` or newer.

### Install CMake

The easiest way is via [Homebrew](https://brew.sh):

```bash
brew install cmake
```

Verify:

```bash
cmake --version
```

Confirm the version is **3.20 or higher**.

### Optional: Install Ninja

Ninja is a faster build tool that CMake can use instead of Make:

```bash
brew install ninja
```

---

## Clone and Build

Clone the repository and run the build script:

```bash
git clone https://github.com/zannagames/zanna.git
cd zanna
./scripts/build_zanna_mac.sh
```

The build script will:

1. Detect Apple Clang as the compiler
2. Configure the project with CMake
3. Build all targets in parallel
4. Run the test suite
5. Install binaries to `/usr/local` (you may be prompted for your password)

A successful build ends with output similar to:

```text
[100%] Built target zanna
...
100% tests passed, 0 tests failed
...
Install complete.
```

---

## Verify the Installation

After building, confirm Zanna is working:

```bash
zanna --version
```

You should see the version string (e.g., `zanna v0.2.x-dev`) followed by the IL version. If the command is not found, ensure `/usr/local/bin` is in your `PATH`:

```bash
echo $PATH | tr ':' '\n' | grep /usr/local/bin
```

If missing, add it to your shell profile:

```bash
echo 'export PATH="/usr/local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### Installing a Release Package

The macOS `.pkg` toolchain installer places the toolchain under `/usr/local/zanna`,
adds command symlinks in `/usr/local/bin`, adds CMake discovery wrappers under
`/usr/local/lib/cmake/Zanna`, registers `.zia`, `.bas`, and `.il` files with the
Zanna Toolchain handler app, and installs `/usr/local/zanna/share/zanna/uninstall.sh`
for local cleanup.

The package does not install Xcode Command Line Tools or CMake. Install those
prerequisites first, then run the `.pkg` and verify `zanna --version`.

Public release packages sign every nested executable with a Developer ID
Application identity, sign the product with a Developer ID Installer identity,
and are notarized, stapled, and Gatekeeper-assessed. The optional DMG is also
notarized/stapled and includes a styled Finder window. You can validate trust
before installation with:

```bash
pkgutil --check-signature Zanna.pkg
spctl --assess --verbose=2 --type install Zanna.pkg
xcrun stapler validate Zanna.pkg
```

To remove a package installed locally, run:

```bash
sudo /usr/local/zanna/share/zanna/uninstall.sh
```

See the [installer and package release guide](../installer-release.md) for
package generation, minimum-OS controls, checksums, and disposable-host
lifecycle validation.

---

## Your First Program

Create a file called `hello.zia` with the following content:

```zia
module Hello;

bind Zanna.Terminal;

func start() {
    Say("Hello, World!");
}
```

Run it:

```bash
zanna run hello.zia
```

**Expected output:**

```text
Hello, World!
```

**What this program does:**

- `module Hello;` declares the module name (required in every Zia file)
- `bind Zanna.Terminal;` imports the Terminal module so you can call its functions directly
- `func start()` is the program entry point (like `main()` in C)
- `Say()` prints a line of text to the console with a trailing newline

---

## What to Read Next

- **[Zia Tutorial](../tutorials/zia-tutorial.md)** — Learn Zia by example: variables, control flow, functions, classes, and generics
- **[Zia Reference](../languages/zia-reference.md)** — Complete language reference
- **[BASIC Tutorial](../tutorials/basic-tutorial.md)** — Zanna also ships a BASIC frontend
- **[Getting Started (general)](../getting-started.md)** — Project creation with `zanna init`, the REPL, IL programs, and the full command reference

---

## Troubleshooting

### 1. "No developer tools were found" or `clang++: command not found`

**Cause:** Xcode Command Line Tools are not installed.

**Fix:**

```bash
xcode-select --install
```

Accept the license dialog. After installation completes, verify with:

```bash
clang++ --version
```

If you previously had the tools installed but they were removed during a macOS upgrade, this same command reinstalls them.

### 2. CMake version too old

**Symptom:** CMake prints an error like:

```text
CMake Error at CMakeLists.txt:1:
  CMake 3.20 or higher is required.  You are running version 3.16.
```

**Cause:** Your Homebrew CMake package is outdated, or you are using the system CMake from an old Xcode.

**Fix:**

```bash
brew update
brew upgrade cmake
cmake --version
```

Confirm the output shows **3.20** or higher. If you installed CMake outside of Homebrew (e.g., from the CMake `.dmg`), ensure the Homebrew version takes precedence in your `PATH`.

### 3. "Permission denied" during install

**Symptom:** The build script fails at the install step with:

```text
file INSTALL cannot copy file ... Permission denied
```

**Cause:** Installing to `/usr/local` requires administrator privileges. The build script uses `sudo` automatically, but your account must be in the admin group.

**Fix:** Run the build script normally — it will prompt for your password when it reaches the install step. If you prefer to skip installation and run directly from the build directory:

```bash
./build/src/tools/zanna/zanna --version
```

You can add the build directory to your PATH instead:

```bash
export PATH="$PWD/build/src/tools/zanna:$PATH"
zanna --version
```
