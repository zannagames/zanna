---
status: active
audience: public
last-verified: 2026-08-31
---

# Getting Started on Linux

This guide walks you through installing and running the Zanna compiler toolchain on Linux. Commands are provided for both Debian/Ubuntu and Fedora/RHEL families.

---

## Prerequisites

| Tool | Minimum Version | Purpose |
|------|-----------------|---------|
| C++20 compiler | Clang 14+ or GCC 11+ | Compile Zanna from source |
| CMake | 3.20 | Build system generator |
| Make | any | Build executor (ships with most distros) |
| Git | any | Clone the repository |

### Install the Compiler and Build Tools

The build script prefers Clang when available, but GCC works equally well.

**Debian / Ubuntu:**

```bash
sudo apt update
sudo apt install clang cmake make git
```

**Fedora / RHEL:**

```bash
sudo dnf install clang cmake make git
```

Verify your compiler version:

```bash
clang++ --version   # should show 14.0 or newer
# or
g++ --version       # should show 11 or newer
```

Verify CMake:

```bash
cmake --version     # must be 3.20 or higher
```

### Optional: Graphics and Audio Libraries

The core compiler, VM, and language frontends build and run with no extra packages.

**Graphics** needs nothing at build time in the default configuration. The native
Wayland adapter is always compiled and `dlopen`s `libwayland-client`,
`libxkbcommon`, and `libwayland-cursor` at runtime; the 3D backend resolves
`libEGL` and `libwayland-egl` the same way. The X11 fallback adapter is added
only when X11 development headers are present; without them you get a valid
Wayland-only build.

**Audio** does need ALSA development headers at configure time. Without them CMake
prints `ZannaAUD: disabled` and the audio library is omitted.

**Debian / Ubuntu:**

```bash
sudo apt install libasound2-dev     # audio
sudo apt install libx11-dev         # optional X11 fallback for graphics
```

**Fedora / RHEL:**

```bash
sudo dnf install alsa-lib-devel     # audio
sudo dnf install libX11-devel       # optional X11 fallback for graphics
```

---

## Clone and Build

Clone the repository and run the build script:

```bash
git clone https://github.com/zannagames/zanna.git
cd zanna
./scripts/build_zanna_linux.sh
```

The build script will:

1. Detect your compiler (prefers Clang, falls back to GCC)
2. Configure the project with CMake
3. Build all targets in parallel (using all available cores)
4. Run the test suite
5. Install binaries to `/usr/local` (prompts for `sudo` if not root)

A successful build ends with output similar to:

```text
[100%] Built target zanna
...
100% tests passed, 0 tests failed out of <N>
...
[build_zanna] Install complete
```

---

## Verify the Installation

After building, confirm Zanna is working:

```bash
zanna --version
```

You should see the version string (e.g., `zanna v0.3.1-snapshot`) followed by the build's source identity and the IL version. If the command is not found, ensure `/usr/local/bin` is in your `PATH`:

```bash
echo $PATH | tr ':' '\n' | grep /usr/local/bin
```

If missing, add it to your shell profile:

```bash
echo 'export PATH="/usr/local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

### Installing a Release Package

Release `.deb` and `.rpm` toolchain packages install `zanna`, headers, runtime archives,
CMake package files, manpages, and desktop/MIME associations. Install them through the
distribution package manager so runtime dependencies are resolved and developer
prerequisites are offered. CMake, `make`, a C++ compiler, and cache utilities are
recommendations rather than hard dependencies, so minimal environments are not
over-constrained:

```bash
# Debian / Ubuntu
sudo apt install ./zanna_<version>_amd64.deb

# Fedora / RHEL
sudo dnf install ./zanna-<version>-1.x86_64.rpm
```

Portable `.tar.gz` packages include `install.sh`, `uninstall.sh`, and an install
manifest. `install.sh` defaults to `/usr/local`, honors `PREFIX` and `DESTDIR`,
and removes stale files from the previous Zanna tarball manifest before copying
the new payload in a rollback-capable transaction. A FUSE-less `.run` bundle is
also available through `zanna install-package --target linux-bundle`; it verifies
its payload and reuses a content-addressed private XDG cache.

See the [installer and package release guide](../installer-release.md) for
checksums, OpenPGP release signing, and disposable-host upgrade/uninstall tests.

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

### 1. "No C++ compiler found" or CMake generator error

**Symptom:** CMake exits with:

```text
CMake Error: CMAKE_CXX_COMPILER not set, not able to find compiler
```

**Cause:** Neither Clang nor GCC is installed. The build script selects `clang`
when it is on `PATH` and otherwise falls back to `gcc`; when neither is present
it leaves the choice to CMake, which then fails to configure.

**Fix:**

```bash
# Debian / Ubuntu
sudo apt install clang
# or
sudo apt install g++-11

# Fedora / RHEL
sudo dnf install clang
# or
sudo dnf install gcc-c++
```

After installing, re-run the build script. Verify with `clang++ --version` or `g++ --version`.

### 2. "X11 development files unavailable" or "ALSA not found" messages

**Symptom:** During the CMake configure step you see status lines like:

```text
-- ZannaGFX: X11 development files unavailable; AUTO will use Wayland only
-- ZannaAUD: disabled (ALSA not found; install libasound2-dev/alsa-lib-devel or set ZANNA_AUDIO_MODE=OFF)
```

**Cause:** X11 or ALSA development headers are not installed. The first message is
informational — the default `AUTO` graphics build still has full native Wayland
support and only loses the X11 fallback. The second means audio is omitted entirely.

**Fix (if you need the X11 fallback or audio):**

```bash
# Debian / Ubuntu
sudo apt install libx11-dev libasound2-dev

# Fedora / RHEL
sudo dnf install libX11-devel alsa-lib-devel
```

Configure with `-DZANNA_AUDIO_MODE=REQUIRE` to turn a missing ALSA into a hard
configure error rather than a silent omission.

Then clean and rebuild:

```bash
rm -rf build
./scripts/build_zanna_linux.sh
```

If you only need the compiler, VM, and language frontends, these warnings are safe to ignore.

### 3. CMake version too old

**Symptom:** CMake prints:

```text
CMake Error at CMakeLists.txt:21 (cmake_minimum_required):
  CMake 3.20 or higher is required.  You are running version 3.16.
```

**Cause:** Older LTS distributions (e.g., Ubuntu 20.04) ship CMake 3.16, which is below the 3.20 minimum.

**Fix — Option A: Install from the Kitware APT repository (Debian/Ubuntu):**

```bash
sudo apt install gpg wget
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ focal main' | sudo tee /etc/apt/sources.list.d/kitware.list
sudo apt update
sudo apt install cmake
```

**Fix — Option B: Install via snap:**

```bash
sudo snap install cmake --classic
```

**Fix — Option C (Fedora):** Fedora typically ships a recent CMake. If yours is outdated:

```bash
sudo dnf upgrade cmake
```

After upgrading, verify with `cmake --version` and re-run the build.
