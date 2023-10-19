#!/usr/bin/env python

# MIT License
#
# Copyright (c) 2023 Davidson Francis <davidsondfgl@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import sys
import os
import subprocess

# Step a: Read symbols from the input text file
if len(sys.argv) != 3:
	sys.stderr.write("Usage: {} input_symbols.txt output_elf"
		.format(sys.argv[0]))
	sys.exit(1)

input_file = sys.argv[1]
output_elf = sys.argv[2]

symbol_data = []
with open(input_file, "r") as file:
	for line in file:
		line = line.strip()
		if not line:
			continue  # Skip empty lines

		# Remove comments that start with '#' (if any)
		line, _, comment = line.partition('#')

		parts = line.split()
		if len(parts) == 2:
			address, symbol_name = parts
			symbol_data.append((address, symbol_name))

# Step b: Create a new ELF assembly file
asm_code = """.section .text
.globl _start
_start:
nop
"""

asm_file = ".asm.s"

with open(asm_file, "w") as asm:
	asm.write(asm_code)

# Step c: Build the ELF assembly file
subprocess.call(["as", asm_file, "-o", ".asm.o"])
subprocess.call(["ld", ".asm.o", "-o", output_elf, "-Ttext=0"])

# Step d: Keep only debug symbols
subprocess.call(["objcopy", "--only-keep-debug", output_elf])

# Step e: Add symbols to the ELF
add_symbol_args = []
for address, symbol_name in symbol_data:
	add_symbol_args.extend(["--add-symbol", "{}=.text:{}"
		.format(symbol_name, address)])

add_symbol_args.insert(0, "objcopy")
add_symbol_args.extend([output_elf, output_elf])

subprocess.call(add_symbol_args)

print("New ELF file with symbols created as {}".format(output_elf))
