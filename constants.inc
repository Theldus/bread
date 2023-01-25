; --------------------------------
; Macros
; --------------------------------
%macro outbyte 2
	mov ax, %2
	mov dx, %1
	out dx, al
%endmacro
%macro inputb 1
	mov dx, %1
	in al, dx
%endmacro
%macro dbg 0
	xchg bx, bx
%endmacro
%macro iowait 0
	xor ax, ax
	mov dx, 0x80
	out dx, al
%endmacro
%macro uart_write_dword 1
	mov ebx, %1
	call uart_write_byte
	shr ebx, 8
	call uart_write_byte
	shr ebx, 8
	call uart_write_byte
	shr ebx, 8
	call uart_write_byte
%endmacro
%macro uart_write_word 1
	mov bx, %1
	call uart_write_byte
	shr bx, 8
	call uart_write_byte
%endmacro

%macro exit_int4 0
	%ifndef UART_POLLING
		jmp .exit
	%else
		jmp read_uart
	%endif
%endmacro

%macro true_exit_int4 0
	%ifndef UART_POLLING
		jmp .exit
	%else
		jmp exit_int1
	%endif
%endmacro

; --------------------------------
; Constants
; --------------------------------

; UART
; --------------
UART_CLOCK_SIGNAL equ 1843200
UART_BASE         equ 0x3F8
UART_BAUD         equ 9600
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
MSG_SINGLE_STEP      equ 0xC8
MSG_READ_MEM         equ 0xD8
MSG_CONTINUE         equ 0xE8

; States
; ------
STATE_DEFAULT         equ 0x01 ; Waits for a new command
STATE_READ_MEM        equ 0x02 ; Read memory state
