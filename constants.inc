; MIT License
;
; Copyright (c) 2023 Davidson Francis <davidsondfgl@gmail.com>
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in all
; copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
; SOFTWARE.

; --------------------------------
; Macros
; --------------------------------

;
; Generic macro to start in a given state and read
; N bytes. After that, jump to the proper state
;
; Parameters:
;   first:  state name label, like: reg_write
;   second: state name, like: STATE_REG_WRITE_PARAMS
;   third:  how many bytes to read
;
; Note: Please note that should be added an entry in
; the jump table called:
;     .state_start_<first_param>
; and later, in '.check_other_states' a jump to:
;     .state_<first_param>_params
;
%macro define_start_and_params_state 3
	;
	; Start of state
	;
.state_start_%1:
	mov byte [cs:byte_counter], 0
	mov byte [cs:state], %2
	jmp read_uart

	;
	; Obtains register offset and value
	;
.state_%1_params:
	; Save new byte read
	movzx bx, byte [cs:byte_counter]
	mov   byte [cs:read_mem_params+bx], al
	inc   byte [cs:byte_counter]

	; Check if we read everything
	cmp byte [cs:byte_counter], %3
	je  .state_%1
	jmp read_uart
%endmacro

;
; Output a byte to an I/O port
; Parameters:
;    first:  I/O port
;    second: byte
;
%macro outbyte 2
	mov ax, %2
	mov dx, %1
	out dx, al
%endmacro

;
; Read from an I/O into al
; Parameters:
;   first: I/O port
;
%macro inputb 1
	mov dx, %1
	in al, dx
%endmacro

; Bochs debugger magic breakpoint
%macro dbg 0
	xchg bx, bx
%endmacro

; Wait a little bit for I/O device to complete
; operation
%macro iowait 0
	xor ax, ax
	mov dx, 0x80
	out dx, al
%endmacro

; Push/pop regs from handler
; Both int1 and int4 have the same handler, and
; thanks to this, one function can jump to another
; without worrying about the stack layout
%macro push_regs 0
	push ss
	push ds
	push es
	push fs
	push gs
	pushad
%endmacro
%macro pop_regs 0
	popad
	pop gs
	pop fs
	pop es
	pop ds
	pop ss
%endmacro

; --------------------------------
; Constants
; --------------------------------

; Misc
; ----
EFLAGS_TF      equ (1<<8)
EFLAGS_IF      equ (1<<9)
DR7_LE_GE      equ 0x700
DR7_LE_GE_L0   equ 0x701
DR7_LE2_RW     equ (3<<24)
DR7_LE2_4byte  equ (3<<26)
DR7_L0         equ (1<<0)
DR7_L2         equ (1<<4)
STOP_REASON_NORMAL      equ 10
STOP_REASON_WATCHPOINT  equ 20

; Register offsets (push_regs/pop_regs)
; -------------------------------------
EDI_OFF    equ 0
ESI_OFF    equ 4
EBP_OFF    equ 8
ESP_OFF    equ 12
EBX_OFF    equ 16
EDX_OFF    equ 20
ECX_OFF    equ 24
EAX_OFF    equ 28
 GS_OFF    equ 32
 FS_OFF    equ 34
 ES_OFF    equ 36
 DS_OFF    equ 38
 SS_OFF    equ 40
EIP_OFF    equ 42
CS_OFF     equ 44
EFLAGS_OFF equ 46

; UART
; --------------
UART_CLOCK_SIGNAL equ 1843200
UART_BASE         equ 0x3F8
UART_BAUD         equ 115200 ; set to 9600 if things go wrong
UART_DIVISOR      equ UART_CLOCK_SIGNAL / (UART_BAUD << 4)
UART_RB           equ UART_BASE + 0 ; Receiver Buffer (R).
UART_IER          equ UART_BASE + 1 ; Interrupt Enable Register (RW).
UART_FCR          equ UART_BASE + 2 ; FIFO Control Register (W).
UART_LCR          equ UART_BASE + 3 ; Line Control Register (RW).
UART_MCR          equ UART_BASE + 4 ; Modem Control Register (W).
UART_LSR          equ UART_BASE + 5 ; Line Status Register (R).

; Line Control Register values
UART_LCR_DLA   equ 0x80 ; Divisor Latch Access.
UART_LCR_BPC_8 equ 0x3  ; 8 bits per character.

; Modem Control Register values
UART_MCR_OUT2  equ 0x8  ; OUT2 pin
UART_MCR_RTS   equ 0x2  ; Request to Send
UART_MCR_DTR   equ 0x1  ; Data Terminal Ready

; Divisor register
UART_DLB1 equ UART_BASE + 0 ; Divisor Latch LSB (RW).
UART_DLB2 equ UART_BASE + 1 ; Divisor Latch MSB (RW).

; FIFO Control Register bits.
UART_FCR_CLRRECV equ 0x1  ; Clear receiver FIFO.
UART_FCR_CLRTMIT equ 0x2  ; Clear transmitter FIFO.

; FIFO Controle Register bit 7-6 values
UART_FCR_TRIG_1 equ 0x0  ; Trigger level 1-byte.

; Line status register
UART_LSR_TFE equ 0x20 ; Transmitter FIFO Empty.


; PIC
; -------------
PIC1          equ 0x20     ; IO base address for master PIC
PIC2          equ 0xA0     ; IO base address for slave PIC
PIC1_COMMAND  equ PIC1
PIC1_DATA     equ (PIC1+1)
PIC2_COMMAND  equ PIC2
PIC2_DATA     equ (PIC2+1)

; INSNs opcodes
; -------------

STOP_OPC equ 0x90fdebf4 ; (f4) (eb fd) (90)
                        ; f4:    lbl: hlt
                        ; eb fd: jmp lbl
                        ; 90:    nop

; Serial messages
; ---------------

; The machine has stopped in single-step mode!
MSG_ADD_SW_BREAK     equ 0xA8
MSG_REM_SW_BREAK     equ 0xB8
MSG_ADD_HW_WATCH     equ 0xB7
MSG_REM_HW_WATCH     equ 0xC7
MSG_SINGLE_STEP      equ 0xC8
MSG_READ_MEM         equ 0xD8
MSG_CONTINUE         equ 0xE8
MSG_WRITE_MEM        equ 0xF8
MSG_CTRLC            equ 0x03
MSG_OK               equ 0x04
MSG_REG_WRITE        equ 0xA7

; States
; ------
STATE_DEFAULT           equ 0x01 ; Waits for a new command
STATE_READ_MEM          equ 0x02 ; Read memory state
STATE_WRITE_MEM         equ 0X03 ; Write memory state
STATE_WRITE_MEM_PARAMS  equ 0x04 ; Write memory params state
STATE_SW_BREAKPOINT     equ 0x05 ; SW breakpoint state
STATE_REG_WRITE_PARAMS  equ 0x06 ; Reg write parameters
STATE_HW_WATCH          equ 0x07 ; HW watchpoint
