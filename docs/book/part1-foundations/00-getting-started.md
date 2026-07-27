---
status: active
audience: public
last-verified: 2026-07-26
---

# Getting Started

Welcome. If you're reading this, you're about to take your first step into the world of programming.

Maybe you've always been curious about how software works. Maybe you've watched others code and thought it looked like magic. Maybe someone told you programming is hard, only for certain kinds of people, or that you need to be good at math. Maybe you're nervous, excited, or both.

Here's the truth: every single programmer who has ever lived started exactly where you are now. They didn't know what a variable was. They didn't understand why semicolons mattered. They stared at error messages in complete confusion. And then, step by step, they figured it out. You will too.

This chapter will help you set up your tools. I won't lie to you — this setup process can be frustrating. Things might not work the first time. You might encounter cryptic error messages. You might feel like you're doing something wrong. That's normal. Every programmer has been through this, usually multiple times.

But I promise you this: if you're patient, read carefully, and don't give up, you will get through it. And once you do, you'll have everything you need to start your programming journey.

Take your time with this chapter. It's okay if it takes an hour. It's okay if you have to try things twice. The setup is often the hardest part for beginners — not because it's intellectually difficult, but because everything is new and unfamiliar. Once you're set up, everything gets easier.

Let's do this together.

---

## What You Need

Before we dive in, let's talk about the tools you'll need and what each one actually does. Understanding your tools will make everything that follows much clearer.

### A Computer

You need a computer running Windows, macOS, or Linux. It doesn't need to be powerful or new — almost any computer from the last decade will work fine. Programming doesn't require fancy hardware; it requires patience and curiosity.

If you're on a Chromebook, tablet, or phone, the instructions here won't work directly. You'll need access to a traditional computer. Perhaps you can use one at a library, school, or borrow from a friend to get started.

### A Text Editor

You'll need a program where you can write your code. This is called a *text editor* or sometimes a *code editor*.

**What is a text editor?** It's a program for editing plain text files — files that contain only text characters, with no formatting like bold, italics, or fonts. Word processors like Microsoft Word or Google Docs won't work because they add hidden formatting information to files. Code must be pure text.

**Why do we need a special program?** You could technically write code in Notepad (Windows) or TextEdit (Mac), but code editors make programming much easier. They color-code your text so different parts stand out, automatically indent your code, catch some mistakes as you type, and provide helpful features like auto-complete.

Here are some excellent free options:

**Visual Studio Code (VS Code)** — This is the most popular code editor in the world. It's free, works on Windows, macOS, and Linux, and has thousands of extensions to add new features. If you're unsure what to choose, choose this.
- Download from: https://code.visualstudio.com
- After installing, you can optionally install the "Zanna" extension for better syntax highlighting

**Sublime Text** — A fast, elegant editor that many programmers love. It's technically paid software, but the free trial never expires.
- Download from: https://www.sublimetext.com

**Notepad++** — A simple, fast editor for Windows only. Great if you want something lightweight.
- Download from: https://notepad-plus-plus.org

**Which should you choose?** If you have no preference, get VS Code. It's excellent, widely used, and there are countless tutorials and resources for it online. But honestly, any of these will work. You can always switch later. Programmers often try many editors before settling on their favorite.

**Important:** Don't spend hours researching editors. Pick one, install it, and move on. The editor is just a tool — what matters is what you create with it.

### The Terminal (Command Line)

You'll also use something called a *terminal*, *command line*, or *command prompt*. This might be new to you, and that's okay.

**What is a terminal?** It's a text-based way to interact with your computer. Instead of clicking on icons and menus, you type commands. It looks like something from an old hacker movie — a black screen with text.

**Why do we need it?** Many programming tools work through the terminal. It might seem old-fashioned, but it's actually incredibly powerful and efficient once you get used to it. You'll type commands to run your programs, install software, and navigate between folders.

**Where do I find it?**

- **On Mac**: Open Finder, go to Applications > Utilities, and open "Terminal". Or press Cmd+Space, type "Terminal", and press Enter.

- **On Windows**: Press the Windows key, type "cmd" or "Command Prompt", and press Enter. Alternatively, you can use "PowerShell" which is similar but more modern.

- **On Linux**: Press Ctrl+Alt+T, or find "Terminal" in your applications menu.

Don't worry if the terminal looks intimidating. You'll only need a few basic commands, and I'll explain each one as we go.

### The Zanna Toolchain

Finally, you need *Zanna* itself — the programming language you'll be learning.

**What is a toolchain?** A "toolchain" is a collection of programs that work together to turn your code into something the computer can run. The most important part is the *compiler*.

**What is a compiler?** Remember how I said computers only understand numbers? A compiler is a program that translates the code you write (in a human-readable programming language) into the numerical instructions your computer's processor can execute.

Here's the flow:
1. You write code in a text file (like `hello.zia`)
2. You run the Zanna compiler on that file
3. The compiler translates your code into machine instructions
4. The computer runs those instructions
5. You see the result

The Zanna toolchain handles all of this. Let's install it.

---

## Installing a Text Editor

Let's start with the easiest part: installing your text editor. I'll walk you through installing VS Code since it's the most popular choice, but the process is similar for other editors.

### Installing VS Code on Windows

1. **Open your web browser** (Chrome, Firefox, Edge, etc.)

2. **Go to** https://code.visualstudio.com

3. **Click the big blue download button.** It should detect that you're on Windows and offer you the Windows installer.

4. **Wait for the download to complete.** You'll probably see it in the bottom of your browser window or in your Downloads folder. The file will be named something like `VSCodeUserSetup-x64-1.xx.x.exe`.

5. **Double-click the downloaded file** to run the installer.

6. **If Windows asks "Do you want to allow this app to make changes to your device?"** — Click "Yes". This is normal for installers.

7. **Follow the installation wizard:**
   - Accept the license agreement (check the box, then click Next)
   - Keep the default installation location (just click Next)
   - On the "Select Additional Tasks" page, I recommend checking:
     - "Add 'Open with Code' action to Windows Explorer file context menu"
     - "Add 'Open with Code' action to Windows Explorer directory context menu"
     - "Add to PATH" (this should already be checked)
   - Click Next, then Install

8. **Wait for the installation to complete**, then click Finish.

9. **Open VS Code** by finding it in your Start menu or clicking the icon on your desktop.

Congratulations! You now have a code editor installed.

### Installing VS Code on macOS

1. **Open your web browser**

2. **Go to** https://code.visualstudio.com

3. **Click the download button.** Choose the Mac version. If you have an Apple Silicon Mac (M1, M2, M3, etc.), choose "Apple Silicon". If you're not sure, try Apple Silicon first — most newer Macs have it.

4. **Wait for the download to complete.** The file will be named something like `VSCode-darwin-universal.zip`.

5. **Open your Downloads folder** in Finder.

6. **Double-click the downloaded .zip file** to extract it. You'll see "Visual Studio Code.app" appear.

7. **Drag "Visual Studio Code.app" to your Applications folder.** You can do this by opening a new Finder window, navigating to Applications, and dragging the app over.

8. **Open VS Code** from your Applications folder or by pressing Cmd+Space, typing "Visual Studio Code", and pressing Enter.

9. **Important:** The first time you open VS Code, macOS might say it's from an "unidentified developer" or ask if you're sure you want to open it. Click "Open" to proceed. This is normal for apps downloaded from the internet rather than the App Store.

You now have VS Code installed!

### Installing VS Code on Linux

The process varies by distribution, but here are the most common methods:

**On Ubuntu/Debian:**
1. Open a terminal
2. Run these commands one at a time:
   ```bash
   sudo apt update
   sudo apt install software-properties-common apt-transport-https wget
   wget -q https://packages.microsoft.com/keys/microsoft.asc -O- | sudo apt-key add -
   sudo add-apt-repository "deb [arch=amd64] https://packages.microsoft.com/repos/vscode stable main"
   sudo apt update
   sudo apt install code
   ```
3. If prompted for your password, type it (you won't see the characters as you type — that's normal)

**Or download directly:**
1. Go to https://code.visualstudio.com
2. Download the .deb file (for Ubuntu/Debian) or .rpm file (for Fedora/RHEL)
3. Double-click the downloaded file to install via your software center

After installation, you can open VS Code by typing `code` in your terminal or finding it in your applications menu.

---

## Understanding the Terminal

Before we install Zanna, let's get comfortable with the terminal. This is often the scariest part for beginners, but I promise it's not as complicated as it looks.

### Opening the Terminal

**On Windows:**
1. Press the Windows key on your keyboard
2. Type `cmd`
3. Press Enter

A black window will appear with some text and a blinking cursor. This is the Command Prompt.

**On macOS:**
1. Press Cmd+Space to open Spotlight
2. Type `Terminal`
3. Press Enter

A window will appear, usually with a white or light background, containing some text and a blinking cursor.

**On Linux:**
1. Press Ctrl+Alt+T

Or find "Terminal" in your applications menu.

### What You're Looking At

When the terminal opens, you'll see something like this:

**On macOS/Linux:**
```text
username@computername ~ %
```

**On Windows:**
```text
C:\Users\YourName>
```

This is called the *prompt*. It's waiting for you to type a command. The information before the cursor tells you who you are and where you are in the file system.

### Your First Commands

Let's try a few commands to get comfortable. Type each command and press Enter.

**See where you are:**
```bash
pwd
```
(On Windows, use `cd` by itself instead)

This shows your current directory (folder). You'll probably see something like `/Users/yourname` (Mac), `/home/yourname` (Linux), or `C:\Users\YourName` (Windows).

**See what files are here:**
```bash
ls
```
(On Windows, use `dir` instead)

This lists all the files and folders in your current location.

**Move to a different folder:**
```bash
cd Documents
```

This changes your directory to the Documents folder. You can replace "Documents" with any folder name.

**Go back up one level:**
```bash
cd ..
```

The `..` means "parent directory" — the folder that contains the current folder.

**Go to your home folder:**
```bash
cd ~
```
(On Windows, use `cd %USERPROFILE%` instead)

The `~` is a shortcut for your home folder.

### Don't Panic

If you type a command wrong, the terminal will show an error message. This is fine! Nothing bad has happened. Read the error, figure out what went wrong (usually a typo), and try again.

If you ever feel stuck or want to cancel what you're doing, press Ctrl+C. This is the universal "stop" command.

---

## Installing Zanna

Now for the main event: installing the Zanna toolchain. This is more involved than installing a regular application, but I'll guide you through every step.

### Prerequisites: What You Need First

Before installing Zanna, you need a few other tools installed on your system. These are common development tools that many programming languages require.

**On macOS:**

You need the Xcode Command Line Tools. Open Terminal and type:
```bash
xcode-select --install
```

A popup will appear asking if you want to install the command line developer tools. Click "Install" and wait for it to complete. This might take several minutes.

If you see a message saying the tools are already installed, great! You can skip this step.

**On Linux (Ubuntu/Debian):**

Open Terminal and run:
```bash
sudo apt update
sudo apt install build-essential git cmake
```

When prompted for your password, type it and press Enter. When asked if you want to continue, type `y` and press Enter.

**On Linux (Fedora/RHEL):**

Open Terminal and run:
```bash
sudo dnf groupinstall "Development Tools"
sudo dnf install git cmake
```

**On Windows:**

You need Visual Studio Build Tools (or Visual Studio) with the C++ workload, plus CMake and Git. The [Windows setup guide](../../getting-started/windows.md) walks through each installer step by step.

### Downloading Zanna

Open your terminal and navigate to a folder where you want to keep the Zanna source code. I recommend creating a folder for development tools:

**On macOS/Linux:**
```bash
cd ~
mkdir -p dev
cd dev
```

These commands:
1. Go to your home folder
2. Create a folder called "dev" if it doesn't exist (the `-p` flag prevents an error if it already exists)
3. Move into that folder

Now, download Zanna using Git:
```bash
git clone https://github.com/zannagames/zanna.git
```

**What is Git?** Git is a version control system — it tracks changes to code over time and lets multiple people collaborate on the same project. The `git clone` command downloads a copy of a project from the internet. Almost all open-source software is distributed this way.

You'll see output showing the download progress. Wait for it to complete.

**If you see "git: command not found":**

Git isn't installed. Install it:
- **macOS**: It should have been installed with Xcode Command Line Tools. Try running `xcode-select --install` again.
- **Linux**: Run `sudo apt install git` (Ubuntu/Debian) or `sudo dnf install git` (Fedora)
- **Windows**: Download from https://git-scm.com and install it

After installing Git, try the `git clone` command again.

### Building Zanna

Once the download completes, you need to *build* Zanna — compile its source code into runnable programs.

Enter the Zanna directory:
```bash
cd zanna
```

Now run the build script for your platform:
```bash
# macOS
./scripts/build_zanna_mac.sh

# Linux
./scripts/build_zanna_linux.sh
```

On Windows, run `.\scripts\build_zanna_win.ps1` from a Developer PowerShell
(see the [Windows setup guide](../../getting-started/windows.md)).

**What does this do?**

The build script:
- Configures the project using CMake
- Compiles all the code
- Runs the test suite to verify everything works
- Installs the tools (to `/usr/local` on macOS/Linux) so you can use them from anywhere

**This step might take several minutes.** You'll see a lot of text scrolling by — that's normal. Don't worry about understanding it all. What matters is that it completes without errors.

**What success looks like:**

When it's done, you'll see your command prompt again, and there should be no red error messages at the end. You might see some yellow warnings — those are generally fine to ignore.

**What failure looks like:**

If something goes wrong, you'll see error messages, often in red. Common issues:

- **"cmake: command not found"**: CMake isn't installed. Install it:
  - macOS: `brew install cmake` (requires Homebrew) or download from https://cmake.org
  - Linux: `sudo apt install cmake` or `sudo dnf install cmake`

- **"No CMAKE_CXX_COMPILER could be found"**: You don't have a C++ compiler. Make sure you installed the build prerequisites above.

- **Errors about missing packages**: Some dependency is missing. Read the error message — it usually tells you what's needed.

If you get stuck, try searching the error message online. Chances are someone else has encountered the same problem.

### Making Zanna Available Everywhere

The build script installs the tools into `/usr/local/bin` on macOS and Linux, which is already on your PATH.

**What is PATH?** When you type a command like `zanna`, your computer searches through a list of folders to find a program with that name. This list is called the PATH. Because the installer places Zanna's tools in a standard location, you normally don't have to configure anything.

If `zanna --version` reports "command not found" after the build finishes, your shell may not include `/usr/local/bin`. Add it:

```bash
echo 'export PATH="/usr/local/bin:$PATH"' >> ~/.zshrc   # or ~/.bashrc on Linux
source ~/.zshrc
```

The platform setup guides cover this and other install issues in more depth: [macOS](../../getting-started/macos.md) · [Linux](../../getting-started/linux.md) · [Windows](../../getting-started/windows.md).

---

## Testing Your Setup

Let's verify everything works. This is an important step — don't skip it!

### Test 1: Check Zanna Version

Open a new terminal window (important: new window, to make sure your PATH changes took effect), and type:

```bash
zia --version
```

**What you should see:**
```text
zia v0.2.99.20260704
Zia Compiler
IL version: 0.3.0
```

(The exact version number will vary depending on when you built Zanna.)

**If you see "command not found":**

Let's debug:

1. First, check if the tools were installed:
   ```bash
   ls /usr/local/bin/zia /usr/local/bin/zanna
   ```

   If this says "No such file or directory", the build didn't complete its install step. Go back to the building step and check for errors near the end of the output.

2. If the files exist, your PATH doesn't include `/usr/local/bin`. Check with:
   ```bash
   echo $PATH
   ```

   If `/usr/local/bin` isn't in the output, add it (see "Making Zanna Available Everywhere" above).

3. As a workaround, you can always run Zanna with the full path:
   ```bash
   /usr/local/bin/zia --version
   ```

### Test 2: Create and Run a Test Program

Let's write a tiny program to make sure the full workflow works.

**Create a test folder:**
```bash
mkdir -p ~/zanna-projects
cd ~/zanna-projects
```

**Create a test file:**

Open your text editor (VS Code or whichever you installed). Create a new file and type:

```zia
module Test;

func start() {
    Zanna.Terminal.Say("Setup complete!");
}
```

Save this file as `test.zia` in your `zanna-projects` folder.

**How to save the file:**
- In VS Code: File > Save (or Ctrl+S / Cmd+S), navigate to your `zanna-projects` folder, name the file `test.zia`, and click Save
- Make sure the filename ends with `.zia`, not `.zia.txt`

**Run the program:**

In your terminal (make sure you're in the zanna-projects folder):
```bash
zanna run test.zia
```

**What you should see:**
```text
Setup complete!
```

If you see those words, congratulations! Your development environment is fully set up and working.

### Test 3: Verify Your Editor Setup

While you have your test file open in your editor, check these things:

**Line numbers should be visible.** You should see numbers (1, 2, 3, etc.) along the left side of your code. If not:
- In VS Code: Go to View > Toggle Line Numbers
- Or search in Settings (Ctrl+, or Cmd+,) for "line numbers"

**The code should be colorized.** Different parts of your code should appear in different colors — keywords like `module` and `func` in one color, the string `"Setup complete!"` in another. This is called *syntax highlighting*. If everything is the same color, your editor might not recognize the `.zia` file type, but that's okay for now.

---

## Troubleshooting Common Problems

Here are solutions to problems that commonly trip up beginners:

### "command not found" (for zia)

**What it means:** Your computer doesn't know where to find the Zanna program.

**Solutions:**

1. **Did you complete the build?** Make sure the platform build script (`./scripts/build_zanna_mac.sh` or `./scripts/build_zanna_linux.sh`) finished without errors, including its install step.

2. **Does your PATH include `/usr/local/bin`?** Check with `echo $PATH`; if it's missing, see "Making Zanna Available Everywhere" above.

3. **Are you in a new terminal window?** If you just edited your shell configuration, open a new terminal (or run `source ~/.zshrc` / `source ~/.bashrc`) so the change takes effect.

4. **Use the full path:** If all else fails, you can always use the full path:
   ```bash
   /usr/local/bin/zia yourprogram.zia
   ```

### "No such file or directory" (for your code file)

**What it means:** Zanna can't find the file you're trying to run.

**Solutions:**

1. **Check your current directory:** Type `pwd` to see where you are, and `ls` to see what files are in that folder. Is your file there?

2. **Navigate to the right folder:** Use `cd` to move to the folder containing your file:
   ```bash
   cd ~/zanna-projects
   ```

3. **Check the filename:** Make sure you saved the file with the `.zia` extension, not `.zia.txt`. In some editors (especially on Windows), you might need to select "All Files" in the save dialog to avoid this.

4. **Use the full path to the file:**
   ```bash
   zia ~/zanna-projects/test.zia
   ```

### "Permission denied"

**What it means:** You don't have permission to run the program or access the file.

**Solutions:**

1. **On macOS/Linux**, if a file you created can't be read or written, check that it lives in a folder you own (like your home directory), not a system folder.

2. **On macOS**, if you see a security warning about an unidentified developer, go to System Preferences > Security & Privacy and click "Allow Anyway".

3. **Don't use sudo with zanna.** You shouldn't need administrator privileges to run your programs.

### "Syntax error" when running your code

**What it means:** There's a mistake in your code.

**Solutions:**

1. **Compare character by character** with the example code. Common mistakes:
   - Missing semicolons (`;`)
   - Wrong type of quotes — use straight quotes (`"`) not curly quotes (" ")
   - Missing or mismatched braces (`{` and `}`)
   - Typos in keywords (`module`, `func`, `start`)

2. **Check the error message.** It usually tells you the line number where the problem was detected. Look at that line and the line before it.

### Build errors (during cmake --build)

**What it means:** Something went wrong while compiling Zanna.

**Solutions:**

1. **Make sure prerequisites are installed.** On macOS, run `xcode-select --install`. On Linux, make sure you have build-essential/Development Tools installed.

2. **Check for specific error messages.** Search online for the exact error text.

3. **Try a clean build:**
   ```bash
   cd ~/dev/zanna
   rm -rf build
   ./scripts/build_zanna_mac.sh   # or build_zanna_linux.sh
   ```

4. **Check available disk space.** Building software requires several gigabytes of free space.

### The terminal seems frozen or stuck

**What it means:** A program is running and waiting for something.

**Solutions:**

1. **Press Ctrl+C** to cancel the current operation and return to the command prompt.

2. **If that doesn't work**, close the terminal window and open a new one.

3. **This is normal** when running programs that wait for input. Some programs expect you to type something.

---

## The Development Workflow

Now that you're set up, let me explain the workflow you'll use for every program you write. Understanding this cycle is fundamental.

### The Edit-Save-Run Cycle

Every program you create follows these steps:

**1. Edit:** Open your text editor and write code (or modify existing code)

**2. Save:** Save the file (Ctrl+S or Cmd+S). Your changes aren't written to disk until you save!

**3. Run:** In the terminal, execute your program with `zia filename.zia`

**4. Observe:** Look at the output. Did it do what you expected?

**5. Repeat:** Go back to step 1 to fix bugs or add features

You'll do this cycle hundreds of times as you learn. Edit, save, run, observe. It becomes second nature.

### A Common Beginner Mistake

One of the most common mistakes is forgetting to save before running. You make changes in your editor, switch to the terminal, run the program... and it does the same thing as before. That's because your changes are still only in the editor — you forgot to save them to the file.

**Get in the habit:** Every time you switch from your editor to the terminal to run your program, save first. Make it automatic: Ctrl+S (or Cmd+S), then run.

### Organizing Your Files

As you work through this book, you'll create many programs. Keep them organized:

```text
zanna-projects/
    chapter02/
        hello.zia
        greeting.zia
    chapter03/
        variables.zia
        calculator.zia
    experiments/
        random-stuff.zia
```

Use folders to group related files. Give your files descriptive names. Your future self will thank you.

---

## What Each Tool Does (Summary)

Let's recap what all these tools do and how they work together:

| Tool | What It Does | When You Use It |
|------|--------------|-----------------|
| **Text Editor** (VS Code, etc.) | A program for writing and editing code | Every time you write or modify a program |
| **Terminal** | A text-based interface for running commands | To navigate folders, run programs, and use developer tools |
| **Git** | Downloads code from the internet and tracks changes | To download Zanna (and later, to manage your own projects) |
| **CMake** | Prepares software projects for building | Part of the Zanna installation process |
| **Compiler** (inside Zanna) | Translates your code into instructions the computer can execute | Every time you run `zia yourfile.zia` |
| **Zanna** | The programming language and its toolchain | To run your Zanna programs |

The flow looks like this:

```text
You write code     -->    Code is saved    -->    Compiler reads    -->    Compiler outputs    -->    Computer runs
in your editor           to a .zia file        and translates it        machine instructions        the program
```

---

## A Note on Perseverance

Setting up a development environment is often the hardest part of learning to program. Not intellectually hard — just frustrating. There are so many things that can go wrong, so many cryptic messages, so many steps that assume knowledge you don't have yet.

If you've made it this far, you've proven something important about yourself: you can work through frustration. That's perhaps the most important skill in programming. Every programmer, from beginners to experts, regularly faces confusing problems. The ones who succeed are the ones who don't give up.

There will be moments in your programming journey when nothing seems to work. When error messages make no sense. When you've tried everything you can think of and you're ready to throw your computer out the window.

When that happens, remember this: every programmer has been there. The error has a solution. You will find it. Take a break, get some fresh air, maybe sleep on it. Come back with fresh eyes. Ask for help online (places like Stack Overflow are full of people who love helping beginners). The answer is out there.

The setup is done. The hard part is behind you. Everything from here gets more fun.

---

## A Note on Typing

Throughout this book, you'll see code examples. You might be tempted to copy and paste them. Resist that temptation.

Type the code yourself, character by character.

Yes, it's slower. Yes, you'll make typos. That's exactly the point.

When you type code, your brain processes it differently than when you read it. You notice things you'd skip over while reading. You learn the rhythm of the syntax. Your fingers start to remember common patterns.

Typos are valuable too. When you mistype something and get an error, you learn what that error means. You develop the crucial skill of reading error messages and tracking down problems. Professional programmers make typos constantly — the difference is they know how to fix them quickly.

So please, type the examples. It's slower now but pays off enormously.

---

## Your First Real File

Let's create one more file to make sure everything is ready for Chapter 1.

1. Open your text editor
2. Create a new file
3. Type this exactly:

```zia
module Hello;

func start() {
    Zanna.Terminal.Say("Hello, World!");
}
```

4. Save it as `hello.zia` in your `zanna-projects` folder

5. Open your terminal and navigate to the folder:
```bash
cd ~/zanna-projects
```

6. Run the program:
```bash
zanna run hello.zia
```

You should see:
```text
Hello, World!
```

If you do — congratulations! You're ready to begin. Your development environment is set up, your tools are working, and you've just run your first real Zanna program.

If something went wrong, go back through the troubleshooting section above. The most common issues are:
- Zanna not in your PATH (see "command not found" section)
- Not being in the right directory (see "No such file or directory" section)
- Typos in the code (compare character by character)

---

## Summary

You've accomplished a lot in this chapter:

- **Installed a text editor** where you'll write your code
- **Learned to use the terminal** to navigate folders and run commands
- **Installed the Zanna toolchain** including all its prerequisites
- **Configured your PATH** so you can run Zanna from anywhere
- **Tested your setup** with a working program
- **Learned the edit-save-run workflow** you'll use for every program
- **Understood what each tool does** and how they work together

This setup work might have felt tedious, but it's done now. From here forward, you'll spend your time actually programming — the fun part.

---

## What's Next

You're set up and ready to go. But before we write more programs, let's talk about what computers actually do and how they work. Understanding the machine you're programming will make everything else make sense.

*Ready? Let's learn what computers actually do.*

*[Continue to Chapter 1: The Machine](01-the-machine.md)*
