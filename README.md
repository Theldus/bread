# 🍞 BREAD
[![License: MIT](https://img.shields.io/badge/License-MIT-orange.svg)](https://opensource.org/licenses/MIT)

BREAD (BIOS Reverse Engineering & Advanced Debugging) is an 'injectable' real-mode x86 debugger that can debug arbitrary real-mode code (on real HW) from another PC via serial cable.

## Introduction
BREAD emerged from many failed attempts to reverse engineer legacy BIOS. Given that the vast majority -- if not all -- BIOS analysis is done statically using disassemblers, understanding the BIOS becomes extremely difficult, since there's no way to know the value of registers or memory in a given piece of code.

Despite this, BREAD can also debug arbitrary code in real-mode, such as bootable code or DOS programs too.

Quick demo:

https://user-images.githubusercontent.com/8294550/217709970-9007a1e3-7352-470d-a22f-cbb5219d5547.mp4
<p align="center">
<a href="https://www.youtube.com/watch?v=G4ex6_eUP0c" target="_blank">
Changing CPU string name via BREAD
</a></br>
</p>

## How it works?
This debugger is divided into two parts: the debugger (written entirely in assembly and running on the hardware being debugged) and the bridge, written in C and running on Linux.

The debugger is the injectable code, written in 16-bit real-mode, and can be placed within the BIOS ROM or any other real-mode code. When executed, it sets up the appropriate interrupt handlers, puts the processor in single-step mode, and waits for commands on the serial port.

The bridge, on the other hand, is the link between the debugger and GDB. The bridge communicates with GDB via TCP and forwards the requests/responses to the debugger through the serial port. The idea behind the bridge is to remove the complexity of GDB packets and establish a simpler protocol for communicating with the machine. In addition, the simpler protocol enables the final code size to be smaller, making it easier for the debugger to be injectable into various different environments.

As shown in the following diagram:
```R
    +---------+ simple packets +----------+   GDB packets  +---------+                                       
    |         |--------------->|          |--------------->|         |                                       
    |   dbg   |                |  bridge  |                |   gdb   |
    |(real HW)|<---------------| (Linux)  |<---------------| (Linux) |
    +---------+    serial      +----------+       TCP      +---------+
```

## Features
By implementing the GDB stub, BREAD has many features out-of-the-box. The following commands are supported:

- Read memory (via [x], [dump], [find], and relateds)
- Write memory (via [set], [restore], and relateds)
- Read and write [registers]
- Single-Step ([si], stepi) and continue ([c], continue)
- Breakpoints ([b], break)[^bp_note]
- Hardware Watchpoints ([watch] and its siblings)[^watchp_note]

[x]: https://sourceware.org/gdb/onlinedocs/gdb/Memory.html
[c]: https://sourceware.org/gdb/download/onlinedocs/gdb/Continuing-and-Stepping.html
[b]: https://sourceware.org/gdb/onlinedocs/gdb/Set-Breaks.html
[si]: https://sourceware.org/gdb/download/onlinedocs/gdb/Continuing-and-Stepping.html
[set]: https://sourceware.org/gdb/onlinedocs/gdb/Assignment.html
[dump]: https://sourceware.org/gdb/onlinedocs/gdb/Dump_002fRestore-Files.html
[find]: https://sourceware.org/gdb/onlinedocs/gdb/Searching-Memory.html
[watch]: https://sourceware.org/gdb/download/onlinedocs/gdb/Set-Watchpoints.html
[restore]: https://sourceware.org/gdb/onlinedocs/gdb/Dump_002fRestore-Files.html
[registers]: https://sourceware.org/gdb/onlinedocs/gdb/Registers.html#Registers

[^bp_note]: Breakpoints are implemented as hardware breakpoints and therefore have a limited number of available breakpoints. In the current implementation, only 1 active breakpoint at a time!
[^watchp_note]: Hardware watchpoints (like breakpoints) are also only supported one at a time.

## GDB Symbols
Reverse engineering a raw binary, such as a BIOS, in GDB automatically implies not having its original symbols. However, as the RE process progresses, the user/programmer/hacker gains a better understanding of certain parts of the code, and static analysis tools like IDA, Cutter, Ghidra, and others allow for the addition of annotations, comments, function definitions, and more. These enhancements significantly boost the user's productivity.

With this in mind, there is a companion Python script in the project called `symbolify.py`. Given a list of symbols (address label), it generates a minimal ELF file with these symbols added. This ELF can then be loaded into GDB later and used to greatly simplify the debugging process.

The symbol file can include whitespace, blank lines, comments (#), and comments on the address line. Addresses can be in decimal or hexadecimal format, and labels/symbols (separated by one or more whitespace characters) can be of the form [a-z0-9_]+, as in (a real example can be found in [symbols/ami_ipm41d3.txt](symbols/ami_ipm41d3.txt)):

```bash
#
# This is a comment
#
0xdeadbeef my_symbol1

0x123 othersymbol # This function does xyz

# Example with decimal address
456 anotherone
```
### Usage
For example, considering the symbol file available at symbols/ami_ipm41d3.txt, the user can do something like:

```bash
$ ./simbolify.py symbols/ami_ipm41d3.txt ip41symbols.elf
```

Then, load it in GDB as in:
```text
(gdb) add-symbol-file ip41symbols.elf 0
add symbol table from file "ip41symbols.elf" at
	.text_addr = 0x0
(y or n) y
Reading symbols from ip41symbols.elf...
(No debugging symbols found in ip41symbols.elf)
(gdb) p cseg_
cseg_change_video_mode_logo  cseg_get_cpuname             
(gdb) p cseg_
```
Note that even GDB auto-complete works as expected, amazing?

## Limitations
How many? Yes.
Since the code being debugged is unaware that it is being debugged, it can interfere with the debugger in several ways, to name a few:

- Protected-mode jump: If the debugged code switches to protected-mode, the structures for interrupt handlers, etc. are altered and the debugger will no longer be invoked at that point in the code. However, it is possible that a jump back to real mode (restoring the full previous state) will allow the debugger to work again.

- IDT changes: If for any reason the debugged code changes the IDT or its base address, the debugger handlers will not be properly invoked.

- Stack: BREAD uses a stack and assumes it exists! It should not be inserted into locations where the stack has not yet been configured.

For BIOS debugging, there are other limitations such as: it is not possible to debug the BIOS code from the very beggining (bootblock), as a minimum setup (such as RAM) is required for BREAD to function correctly. However, it is possible to perform a "warm-reboot" by setting CS:EIP to `F000:FFF0`. In this scenario, the BIOS initialization can be followed again, as BREAD is already properly loaded. Please note that the "code-path" of BIOS initialization during a warm-reboot may be different from a cold-reboot and the execution flow may not be exactly the same.

## Building
Building only requires GNU Make, a C compiler (such as GCC, Clang, or TCC), NASM, and a Linux machine.

The debugger has two modes of operation: polling (default) and interrupt-based:

### Polling mode
Polling mode is the simplest approach and should work well in a variety of environments. However, due the polling nature, there is a high CPU usage:

#### Building
```bash
$ git clone https://github.com/Theldus/BREAD.git
$ cd BREAD/
$ make
```

### Interrupt-based mode
The interrupt-based mode optimizes CPU utilization by utilizing UART interrupts to receive new data, instead of constantly polling for it. This results in the CPU remaining in a 'halt' state until receiving commands from the debugger, and thus, preventing it from consuming 100% of the CPU's resources. However, as interrupts are not always enabled, this mode is not set as the default option:

#### Building
```bash
$ git clone https://github.com/Theldus/BREAD.git
$ cd BREAD/
$ make UART_POLLING=no
```

## Usage
Using BREAD only requires a serial cable (and yes, your motherboard __has__ a COM header, check the manual) and injecting the code at the appropriate location.

To inject, minimal changes must be made in dbg.asm (the debugger's src). The code's 'ORG' must be changed and also how the code should return (look for "`>> CHANGE_HERE <<`" in the code for places that need to be changed).

### For BIOS (e.g., AMI Legacy):
Using an AMI legacy as an example, where the debugger module will be placed in the place of the BIOS logo (`0x108200` or `FFFF:8210`) and the following instructions in the ROM have been replaced with a far call to the module:
```asm
...
00017EF2  06                push es
00017EF3  1E                push ds
00017EF4  07                pop es
00017EF5  8BD8              mov bx,ax     -┐ replaced by: call 0xFFFF:0x8210 (dbg.bin)
00017EF7  B8024F            mov ax,0x4f02 -┘
00017EFA  CD10              int 0x10
00017EFC  07                pop es
00017EFD  C3                ret
...
```
the following patch is sufficient:
```patch
diff --git a/dbg.asm b/dbg.asm
index caedb70..88024d3 100644
--- a/dbg.asm
+++ b/dbg.asm
@@ -21,7 +21,7 @@
 ; SOFTWARE.
 
 [BITS 16]
-[ORG 0x0000] ; >> CHANGE_HERE <<
+[ORG 0x8210] ; >> CHANGE_HERE <<
 
 %include "constants.inc"
 
@@ -140,8 +140,8 @@ _start:
 
 	; >> CHANGE_HERE <<
 	; Overwritten BIOS instructions below (if any)
-	nop
-	nop
+	mov ax, 0x4F02
+	int 0x10
 	nop
 	nop
```

It is important to note that if you have altered a few instructions within your ROM to invoke the debugger code, they must be restored prior to returning from the debugger.

The reason for replacing these two instructions is that they are executed just prior to the BIOS displaying the logo on the screen, which is now the debugger, ensuring a few key points:

- The logo module (which is the debugger) has already been loaded into memory
- Video interrupts from the BIOS already work
- The code around it indicates that the stack already exists

Finding a good location to call the debugger (where the BIOS has already initialized enough, but not too late) can be challenging, but it is possible.

After this, `dbg.bin` is ready to be inserted into the correct position in the ROM.

### For DOS
Debugging DOS programs with BREAD is a bit tricky, but possible:

#### 1. Edit `dbg.asm` so that DOS understands it as a valid DOS program:
   - Set the ORG to 0x100
   - Leave the useful code away from the beginning of the file (`times`)
   - Set the program output (`int 0x20`)

The following patch addresses this:
```patch
diff --git a/dbg.asm b/dbg.asm
index caedb70..b042d35 100644
--- a/dbg.asm
+++ b/dbg.asm
@@ -21,7 +21,10 @@
 ; SOFTWARE.
 
 [BITS 16]
-[ORG 0x0000] ; >> CHANGE_HERE <<
+[ORG 0x100]
+
+times 40*1024 db 0x90 ; keep some distance,
+                      ; 40kB should be enough
 
 %include "constants.inc"
 
@@ -140,7 +143,7 @@ _start:
 
 	; >> CHANGE_HERE <<
 	; Overwritten BIOS instructions below (if any)
-	nop
+	int 0x20 ; DOS interrupt to exit process
 	nop
```

#### 2. Create a minimal bootable DOS environment and run
Create a bootable FreeDOS (or DOS) floppy image containing just the kernel and the terminal: `KERNEL.SYS` and `COMMAND.COM`. Also add to this floppy image the program to be debugged and the `DBG.COM` (`dbg.bin`).

The following steps should be taken after creating the image:

- Boot it with `bridge` already opened (refer to the next section for instructions).
- Execute `DBG.COM`.
- Once execution stops, use GDB to add any desired breakpoints and watchpoints relative to the next process you want to debug. Then, allow the `DBG.COM` process to continue until it finishes.
- Run the process that you want to debug. The previously-configured breakpoints and watchpoints should trigger as expected.

It is important to note that DOS does not erase the process image after it exits. As a result, the debugger can be configured like any other DOS program and the appropriate breakpoints can be set. The beginning of the debugger is filled with NOPs, so it is anticipated that the new process will not overwrite the debugger's memory, allowing it to continue functioning even after it appears to be "finished". This allows BREaD to debug other programs, including DOS itself.

### Bridge
Bridge is the glue between the debugger and GDB and can be used in different ways, whether on real hardware or virtual machine.

Its parameters are:
```text
Usage: ./bridge [options]
Options:
  -s Enable serial through socket, instead of device
  -d <path> Replaces the default device path (/dev/ttyUSB0)
            (does not work if -s is enabled)
  -p <port> Serial port (as socket), default: 2345
  -g <port> GDB port, default: 1234
  -h This help

If no options are passed the default behavior is:
  ./bridge -d /dev/ttyUSB0 -g 1234

Minimal recommended usages:
  ./bridge -s (socket mode, serial on 2345 and GDB on 1234)
  ./bridge    (device mode, serial on /dev/ttyUSB0 and GDB on 1234)
```

#### Real hardware
To use it on real hardware, just invoke it without parameters. Optionally, you can change the device path with the `-d` parameter:

##### Execution flow:
1. Connect serial cable to PC
2. Run bridge (`./bridge` or `./bridge -d /path/to/device`)
3. Turn on the PC to be debugged
4. Wait for the message: `Single-stepped, you can now connect GDB!` and then launch GDB: `gdb`.

#### Virtual machine
For use in a virtual machine, the execution order changes slightly:

##### Execution flow:
1. Run bridge (`./bridge` or `./bridge -d /path/to/device`)
2. Open the VM[^vm_note] (such as: `make bochs` or `make qemu`)
3. Wait for the message: `Single-stepped, you can now connect GDB!` and then launch GDB: `gdb`.

_In both cases, be sure to run GDB inside the BRIDGE root folder, as there are auxiliary files in this folder for GDB to work properly in 16-bit._

[^vm_note]: Please note that debug registers do not work by default on VMs. For bochs, it needs to be compiled with the `--enable-x86-debugger=yes` flag. For Qemu, it needs to run with KVM enabled: `--enable-kvm` (`make qemu` already does this).

## Contributing

BREAD is always open to the community and willing to accept contributions,
whether with issues, documentation, testing, new features, bugfixes, typos, and
etc. Welcome aboard.

## License and Authors

BREAD is licensed under MIT License. Written by Davidson Francis and
(hopefully) other
[contributors](https://github.com/Theldus/bread/graphs/contributors).
