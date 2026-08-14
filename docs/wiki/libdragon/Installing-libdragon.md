## Overview

Installing libdragon requires two things:

 1. Installing the GCC MIPS64 toolchain (that is the C compiler that can generate MIPS binary)
 2. Compiling libdragon itself (the library), its tools, and its examples

Installing the toolchain can be fast (if you get a binary version) or slow (if you decide to recompile it yourself), but you only need to do this once. There is never a real need to upgrade the toolchain. Once you install it once, it will likely be fine for years to come.

Viceversa, compiling libdragon and its tools takes one minute or so, and you can do this as many times as you want, including upgrading libdragon every day (especially if you follow the [preview branch](https://github.com/DragonMinded/libdragon/wiki/Preview-branch)). You can upgrade libdragon freely and even have ten different versions on your computer. But you still just need one toolchain.

## Option 1: download a prebuilt binary of the toolchain via zip archive or deb/rpm package

**Supported OS:** Windows (requires WSL2 or MSYS2), Linux

Download the [pre-built toolchain, available on GitHub](https://github.com/DragonMinded/libdragon/releases/tag/toolchain-continuous-prerelease) as a release in this repository. There is a ZIP file for Windows, plus a `.deb` and a `.rpm` file for Linux. Additional instructions:

### Windows users with WSL2, Debian-based Linux distros (Ubuntu, etc.):

  1. Make sure the package manager is up to date: `sudo apt-get update`.
  2. Install prerequisites: `sudo apt install build-essential`
  3. Download the [toolchain in .deb format](https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-x86_64.deb) and install it via `sudo dpkg -i gcc-toolchain-mips64-x86_64.deb`.
  4. Close the terminal and open a new one.
  5. Download a copy of libdragon from GitHub (with `git clone https://github.com/DragonMinded/libdragon.git`, for example), then run `./build.sh` to build and install libdragon.

### Windows users with MSYS2:

  1. Download the [native Windows toolchain in ZIP format](https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-win64.zip) and unpack it somewhere (eg: `c:\n64-toolchain`). **Avoid paths with spaces.**
  2. Open the Start menu, type "env", select "Edit the system environment variables" to open the Control Panel, and then click the "Environment Variables..." button to open the "Environment Variables" dialog. Create a new user variable with name `N64_INST` and set the value to the directory where you unpacked the toolchain (eg: `c:\n64-toolchain`). 
  3. Download a copy of libdragon from GitHub (with `git clone https://github.com/DragonMinded/libdragon.git`, for example). 
  4. Open an **MSYS2 UCRT64** terminal (use the Start menu shortcut called like this), inside the terminal move to the directory where you unpacked libdragon (eg: `cd /c/Users/Foo/libdragon`).
  5. Run `pacman -S base-devel mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make git` to install the dependencies
  6. Run `./build.sh` to build and install libdragon.

### Red Hat-based Linux distros (Fedora, etc.):

  1. Install prerequisite standard build tools with `sudo dnf install gcc gcc-c++ make git`.
  2. Download the [toolchain in .rpm format](https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-x86_64.rpm).
  3. Install the downloaded toolchain with `sudo dnf install gcc-toolchain-mips64-x86_64.rpm`. The package will install the toolchain into `/opt/libdragon`, and will also create a script file in `/etc/profile.d` to automatically configure the environment variable `$N64_INST=/opt/libdragon` for you.
  4. Close the terminal and open a new one. The shell should have run the script file and configured the system. Try `echo $N64_INST` to confirm the installation was successful.
  5. Download a copy of libdragon from GitHub (with `git clone https://github.com/DragonMinded/libdragon.git`, for example) and run `./build.sh` to compile and install libdragon itself. The `./build.sh` script will ask for sudo when needed to install into `/opt/libdragon`.

## Option 2: installation with Docker, with the libdragon CLI

**Supported OS:** Windows, Linux, macOS x86/ARM

This option uses Docker as a way to quickly ship a prebuilt toolchain on all operating systems. It does not require you to explicitly learn Docker: you will interact with a libdragon CLI and Docker will be used under the hood.

See [the libdragon CLI](https://github.com/anacierdem/libdragon-docker) to quickly get libdragon up and running. Basically:

1. Make sure that you have Docker installed correctly (on Windows and Mac, use [Docker Desktop](https://www.docker.com/products/docker-desktop/)). You can run `docker system info` to check that it is working correctly.
2. Install the [the libdragon CLI](https://github.com/anacierdem/libdragon-docker).
   You have two options:

   a. Download the [pre-built binary](https://github.com/anacierdem/libdragon-docker/releases/latest), 
      and copy it into some directory which is part of your system PATH.  
   b. Or, if you have `npm` installed (at least verstion 14), run `npm install -g libdragon`.

3. Run `libdragon init` to create a skeleton project. The first time ever you run this, it will also download the libdragon Docker image that contains the toolchain. It will also download libdragon itself and builds it for you.
4. If you want to use the preview branch of libdragon, you can switch to it now as follows:
```
git -C ./libdragon checkout preview
libdragon install
```
5. Run `libdragon make` to compile and build an initial test ROM

If you want, you can also compile and run one of the examples that will be found in `libdragon/examples` in the skeleton project.

Note for Apple Silicon users: we ship and update the docker container image for both x86-64 and arm64, so this option works on Apple Silicon machines too.


## Option 3: compile and install the toolchain from sources

> [!NOTE]
> Building a toolchain from sources can take a long time. Time of 1 hr or more are not unexpected, especially if you don't have the latest gaming PC on the market.

**Supported OS**: Windows via WSL2 or msys2, Linux, macOS x86/arm

These instructions work for Linux, macOS (Intel / Apple Silicon) and Windows with WSL2 or msys2.
WSL1 users must [upgrade to WSL2 first](https://docs.microsoft.com/en-us/windows/wsl/install#upgrade-version-from-wsl-1-to-wsl-2).

1. Export the environment variable N64_INST to the path where you want your toolchain to be installed. For instance: `export N64_INST=/opt/libdragon` or `export N64_INST=/usr/local`.
2. If you are on macOS, make sure [homebrew](https://brew.sh) is installed.
3. Install the following packages that are needed: these are the Debian names, but they should be similar on all systems: `build-essential` (GCC, binutils, make, autotools, etc.), `texinfo`, `zlib1g-dev`.
3. Make sure you have at least 7 GB of disk space available (notice that after build, only about 300 MB will be used, but during build a lot of space is required).
4. Enter the `tools` directory and Run `./build-toolchain.sh`, let it build and install the toolchain. The process will take a while depending on your computer (1 hour is not unexpected).
5. Make sure that you still have the `N64_INST` variable pointing to the correct directory where the toolchain was installed (`echo $N64_INST`).
6. Run `./build.sh` at the top-level. This will install libdragon, its tools, and also build all examples.

You are now ready to run the examples on your N64 or emulator.

Once you are sure everything is fine, you can delete the `tools/toolchain/`
directory, where the toolchain was built. This will free around 6 GB of space.
You will only need the installed binaries in the `N64_INST` from now on.
