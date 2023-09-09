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

CC = cc
#CFLAGS += -fsanitize=address
CFLAGS += -MMD -MP -Wall -Wextra
OBJ = gdb.o main.o net.o util.o
DEP = $(patsubst %.d, .%.d, $(OBJ:.o=.d))
BIN = bridge boot.bin dbg.bin bootable.img

# Check if serial or not
ifeq ($(VERBOSE), yes)
	CFLAGS += -DVERBOSE
endif

# Define if polling or not
UART_POLLING ?= yes

ifeq ($(UART_POLLING), yes)
	CFLAGS   += -DUART_POLLING
	ASMFLAGS += -DUART_POLLING
endif

.PHONY: all bochs clean

all: $(BIN)

# C Files
%.o: %.c Makefile
	$(CC) $< $(CFLAGS) -c -o $@ -MF $(patsubst %.d, .%.d, $*.d)

# ASM Files
%.bin: %.asm
	nasm -fbin $< -o $@ $(ASMFLAGS)

dbg.bin: dbg.asm constants.inc

# Main
bridge: $(OBJ)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

# Bootable image
bootable.img: boot.bin dbg.bin
	cat boot.bin dbg.bin > bootable.img

# Run on Bochs and QEMU
bochs: bootable.img
	bochs -q -f .bochsrc.txt
qemu: bootable.img
	qemu-system-i386 -boot a -fda bootable.img \
		-serial tcp:127.0.0.1:2345 -gdb tcp::5678 --enable-kvm


clean:
	$(RM) $(OBJ)
	$(RM) $(BIN)
	$(RM) $(DEP)

-include $(DEP)
