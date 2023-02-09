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

### bbbb
