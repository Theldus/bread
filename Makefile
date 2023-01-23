CC = cc
#CFLAGS += -fsanitize=address
CFLAGS += -MMD -MP
OBJ = gdb.o main.o net.o util.o
DEP = $(patsubst %.d, .%.d, $(OBJ:.o=.d))
BIN = bridge boot.bin dbg.bin bootable.img

USE_SERIAL=no

# Check if serial or not
ifeq ($(USE_SERIAL), yes)
	CFLAGS += -DUSE_SERIAL
endif

.PHONY: all bochs clean

all: $(BIN)

# C Files
%.o: %.c Makefile
	$(CC) $< $(CFLAGS) -c -o $@ -MF $(patsubst %.d, .%.d, $*.d)

# ASM Files
%.bin: %.asm
	nasm -fbin $< -o $@

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
		-serial tcp:127.0.0.1:2345 -display none -s


clean:
	$(RM) $(OBJ)
	$(RM) $(BIN)
	$(RM) $(DEP)

-include $(DEP)
