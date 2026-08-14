This guide shows you how to setup **gdb** to debug a libdragon N64 rom running in the **ares** emulator.

These instructions were tested on Xubuntu 24.04.

1. Make sure you have [the latest build of **ares**](https://ares-emu.net/download)
2. Enable Homebrew mode and GDB debugging in ares settings

![image](https://github.com/user-attachments/assets/4030b5be-26b9-4de9-81e5-b9b9723e7213)
![image](https://github.com/user-attachments/assets/11b4eff1-3a1e-4b80-8783-344f2fd4eb1f)

In this example I also enabled IPv4 mode so the local address will be 127.0.0.1:9123

3. Acquire a **gdb** build with MIPS support.

Your libdragon toolchain install should already have a gdb with MIPS support in `$N64_INST/bin/mips64-elf-gdb`. If not you can install it on Ubuntu `sudo apt install gdb-multiarch` (which I used in this guide) or build it via libdragon's `tools/build_gdb.sh`

4. Build your rom with `make D=1` to enable debugging. This flag is interpreted in the `n64.mk` file to add debug info but it doesn't explicitly change any optimization flags:

```
ifeq ($(D),1)
CFLAGS+=-g3
CXXFLAGS+=-g3
ASFLAGS+=-g
RSPASFLAGS+=-g
LDFLAGS+=-g
endif
```

5. Test that **gdb** works on the command line. Load your ROM in **ares**:

```
ares myrom.z64
```

You should see the message "GDB Listening [::1]:9123" or "GDB Listening 127.0.0.1:9123" on the status bar, depending on if you use IPv6 or not:

![image](https://github.com/user-attachments/assets/5a9a01e4-a30e-41e2-b593-ca7a94d2340c) ![image](https://github.com/user-attachments/assets/fd304367-a71f-485c-b734-09748427cdb3)

In another command-line window, run `gdb-multiarch build/myrom.elf`.

If you see a "safe-path" warning and the symbols aren't loaded, do as it says and create a `~/.config/gdb/gdbinit` file that lists where you expect to load symbols.

```
warning: File "./build/myrom.elf" auto-loading has been declined by your `auto-load safe-path'
set to "$debugdir:$datadir/auto-load".
To enable execution of this file add
	add-auto-load-safe-path /home/user/dev/n64/myproject/build/myrom.elf
```

For example I have my code in `~/dev` so my config looks like this:

```
cat ~/.config/gdb/gdbinit
set auto-load safe-path /home/user/dev/
```

Then try connecting to **ares** with the **gdb** command `target remote 127.0.0.1:9123`

```
(gdb) target remote 127.0.0.1:9123
Remote debugging using 127.0.0.1:9123
0x80031084 in __rsp_check_assert ()
```

Now **ares** should say "GDB Connected":
![image](https://github.com/user-attachments/assets/74ed4dfc-91a6-42e1-90e2-3936497299ac)

We can verify it works by printing a backtrace with `bt`:
```
(gdb) bt
#0  0x80031084 in __rsp_check_assert ()
#1  0x8002ba68 in display_get ()
#2  0x800246c0 in main ()
```

You can continue execution with the `c` command. See [this tutorial by Dragorn421](https://github.com/Dragorn421/z64-romhack-tutorials/blob/master/debugging/gdb/terminal.md#basic-commands) for an introduction to the gdb command line.

Great it works. Let's hook it up to an editor.

## Visual Studio Code

Install [the Microsoft "C/C++" `ms-vscode.cpptools` extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools).

Then in your `launch.json` add the following entry. Click the gear icon in the debugging sidebar tab to access the file:

![image](https://github.com/user-attachments/assets/505d1730-dbe5-4784-ad57-fc04ac22a75f)

This is how my config looks:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "gdb",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceRoot}/build/myrom.elf",
            "miDebuggerServerAddress": "127.0.0.1:9123",
            "miDebuggerPath": "gdb-multiarch",
            "MIMode": "gdb",
            "cwd": "${workspaceFolder}",
            "miDebuggerArgs": "-readnow",
            "stopAtEntry": true,
            "sourceFileMap": {
                "libdragon": "../libdragon",
                "src/t3d": "../tiny3d/src/t3d"
            }
        }
    ]
}
```
The [`sourceFileMap` object](https://code.visualstudio.com/docs/cpp/launch-json-reference#_sourcefilemap) sets replacements of prefixes (keys) of source file paths reported by gdb with the given real paths (values).
In my case I have the LibDragon and Tiny3D repos next to my project repo, so I added relative paths there.

Now when you start **ares** and then run your new launch config, the debugger should break into a random location:

![image](https://github.com/user-attachments/assets/420d1a41-2594-4592-8cb2-5d0238f4deb1)

![image](https://github.com/user-attachments/assets/20a5c265-adac-4608-8250-0ce031488bc1)

Happy debugging! You can learn more about VS Code's debugger again [in a guide by Dragorn421](https://github.com/Dragorn421/z64-romhack-tutorials/blob/master/debugging/gdb/vscode.md).

## CLion

To integrate debugging in CLion, you need to set up an "Embedded GDB Server" run configuration.

Open your project, and select **Run &rarr; Edit Configurations...** from the menu bar. Then click on + to add a configuration and find "Embedded GDB Server".

<img width="322" height="177" alt="Adding the run configuration" src="https://github.com/user-attachments/assets/16511546-a1b5-4cd1-a296-2166f63e510a" />

In the run configuration settings, you need to fill in each field as follows:

<img width="730" height="313" alt="Settings for the run configuration" src="https://github.com/user-attachments/assets/003c5887-effa-4fe5-a5a7-928bbab5c55a" />

What is important to note here is that the executable binary needs to be set to the built ELF file, which by default will be under the `build` directory in your project, not the final ROM image.

On macOS, the path to ares needs to be for the executable inside the app bundle, it will look something like `/Applications/ares.app/Contents/MacOS/ares`.

Using the GDB included with libdragon's toolchain or an otherwise up-to-date gdb-multiarch rather than the bundled GDB is required, as the bundled GDB does not currently seem to be able to resolve symbols in MIPS executables.

In addition to the ares configuration above, you need to enable **Settings &rarr; Boot Options &rarr; Await GDB Client** for the full GDB integration to work, as CLion needs to be able to attach to ares's debug server before the N64 program starts running.

<img width="392" height="228" alt="image" src="https://github.com/user-attachments/assets/fccb2dc2-8e22-4b13-a0db-16e1cad6b134" />

Now, simply click the Debug button in CLion and it will launch ares and attach GDB for you. Happy debugging!

## Troubleshooting
### Constantly trapping

You need to update your **ares** emulator if you get an error like below and can't set any breakpoints.

```
Program received signal SIGTRAP, Trace/breakpoint trap.
0x8002aae4 in __display_callback ()
(gdb) bt
#0  0x8002aae4 in __display_callback ()
#1  0x80026e40 in __MI_handler ()
#2  0x80000628 in notcart ()
Backtrace stopped: frame did not save the PC
```

### No symbols
If **gdb-multiarch** can't find any symbols, it might be out of date and the libdragon toolchain's **gcc** is generating symbols in an unsupported DWARF format.
The "multiarch" version is nothing more than gdb built with more architectures in it. Unfortunately it's difficult to build on your own, so it's better to download a binary. On Ubuntu-based systems you can download a version for a different distro and hope it works despite dependencies being out of date!

If you're running an ancient Ubuntu, you can [download a `.deb` for a newer version](https://packages.ubuntu.com/search?keywords=gdb-multiarch) and extract the binary with
```
cd ~/Downloads
wget http://ge.archive.ubuntu.com/ubuntu/pool/universe/g/gdb/gdb-multiarch_15.1-1ubuntu2_amd64.deb 
ar -xvf gdb-multiarch_15.1-1ubuntu2_amd64.deb
# Warning: The following command generates ./usr/bin directories
tar -xvf data.tar.zst
./usr/bin/gdb-multiarch --version
``` 


