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

[BITS 16]
[ORG 0x0000] ; >> CHANGE_HERE <<

%include "constants.inc"

;
; Debugger entry point
;
; Register usage:
; 0  DS
; 2  EFLAGS
; 4  DI
; 6  SI
; 8  BP
; 10 SP
; 12 BX
; 14 DX
; 16 CX
; 18 AX
; 20 EIP
; 22 CS
;
_start:
	pushad
	pushfd
	push ds

	; Setup UART for early error messages
	call setup_uart

	; Check the current IDT pointer and save
	; our entry to it
	mov ax, cs
	mov ds, ax
	sidt [idtptr]

	; Check data
	mov eax, [idt_base]
	cmp eax, (1<<20)      ; If greater than 1M, error
	jge .greater_than_1M_error

	; If less, we get the SEG:OFF to save our brand
	; new handlers =)

	; Offset
	mov bx, ax

	; Segment
	shr eax, 4
	and eax, 0xF000 ; now fits in AX
	mov ds, ax      ; set our segment
	jmp .configure_handlers

	.greater_than_1M_error:
		mov  ax, cs
		mov  ds, ax
		mov  si, str_idt_gt1MB_error
		call uart_write_string
		jmp  .exit

.configure_handlers:
	; Set int1 handler
	mov word [bx+(1*4)+0], handler_int1
	mov word [bx+(1*4)+2], cs

%ifndef UART_POLLING
	; Serial/COM1 handler
	mov word [bx+(36*4)+0], handler_int4_com1
	mov word [bx+(36*4)+2], cs

	; Configure our PIC to receive ints from COM1
	call setup_pic
	sti
%endif

	; Set DR7 to a known value
	mov eax, DR7_LE_GE ; GE/DE/disabled
	mov DR7, eax

	; Enable TF and IF in backup flags
	mov bp, sp
	or word [bp+2], EFLAGS_TF ; TF
%ifndef UART_POLLING
	or word [bp+2], EFLAGS_IF ; IF
%endif

	xor eax, eax

	; Enable TF right now too
	pushf
	mov bp, sp
	or word [bp], EFLAGS_TF
	popf

	xor dx, dx
.random_label:
	; Random instructions to dbg before return
	; to real code
	nop
	nop
	inc eax
	nop
	nop
	inc dword [cs:random_data] ; Write watchpoint
	nop
	nop
	nop
	mov ebx, dword [cs:random_data] ; Read watchpoint
	nop
	cmp dx, 5
	jne .random_label

.exit:
	; Restore old registers and flags
	pop ds
	popfd
	popad

	; >> CHANGE_HERE <<
	; Overwritten BIOS instructions below (if any)
	nop
	nop
	nop
	nop

	; Return from caller/BIOS
	retf

random_data: dd 0xdeadbeef

; Padding bytes to 'protect' our handler:
; because when single-stepping 'retf'
; we will overwrite the next 3 bytes too
; which might be the handler
nop
nop
nop
nop

;
; INT1 handler
;
;
; Stack order:
; 00 EDI     ---    (TOP)
; 04 ESI        |
; 08 EBP        |
; 12 ESP        | - Saved by us  (32-bit each)
; 16 EBX        |
; 20 EDX        |
; 24 ECX        |
; 28 EAX     ---
; 32  GS     ---
; 34  FS        |
; 36  ES        | - Saved by us  (16-bit each)
; 38  DS        |
; 40  SS     ---
; 42 EIP     ---
; 44 CS         | - Saved for us (16-bit each)
; 46 EFLAGS  ---
;
;
; Obs:
; For Trap: EIP points to the next (not-executed yet)
; instruction!
;
; For Debug Exception:
;   If Instruction: EIP points to the (not-executed yet)
;   instruction.
;
;   If Data: EIP points to the next (not-executed yet)
;   instruction
;
handler_int1:

%ifndef UART_POLLING
	; Check if we should proceed:
	; Since we may also wakeup (from hlt) due to
	; another interrupt, we need to check if our
	; flag allows us to proceed or not.
	cmp byte [cs:should_step], 1
	jne exit_int1_iret
%endif

	; Save everyone
	push_regs

handler_int1_send:
	; Send all regs and stop reason to our bridge
	call send_stop_msg

%ifndef UART_POLLING
	; Overwrite our return instruction by
	;   lbl: hlt + jmp lbl
	; This avoids the code to proceed unless
	; if it really should do so...
	mov bp, sp
	xor ebx, ebx
	mov bx, word [bp+EIP_OFF] ; EIP
	mov ds, word [bp+CS_OFF]  ; CS

	;
	; Backup it!
	; Location:
	;
	mov word [cs:saved_eip], bx
	mov word [cs:saved_cs],  ds

	; Our original insns
	mov eax, dword [ds:bx]
	mov dword [cs:saved_insn], eax

	; Overwrite it!
	mov dword [ds:bx], STOP_OPC

	; Send our original instructions.
	mov ebx, eax
	call uart_write_dword

	; should_step = 0
	mov byte [cs:should_step], 0
%endif

	; Disable breakpoints
	call disable_hw_breakpoints

%ifndef UART_POLLING
	; Disable TF because we shouldn't trigger a SS-trap
	; for hlt or jmp
	mov bp, sp
	and word [ss:bp+EFLAGS_OFF], ~EFLAGS_TF
%endif

%ifdef UART_POLLING
	jmp read_uart
%endif

exit_int1:
	pop_regs
exit_int1_iret:
	iret

;
; Serial/COM1 handler & main state machine
;
; All the commands are done here, even on polling
; mode!
;
; Stack order:
;  Same as int1
;
handler_int4_com1:
	push_regs

	; ACK interrupts, only PIC master is enough to us
	outbyte PIC1_COMMAND, 0x20 ; ACK/EOI

	; Read a single byte from UART to al
read_uart:
	inputb UART_LSR ; Check if there is input available
	bt  ax, 0
%ifdef UART_POLLING
	jnc read_uart
%else
	jnc exit_int4
%endif

	inputb UART_RB
	mov byte [cs:byte_read], al

	; Check state and acts accordingly
	cmp byte [cs:state], STATE_DEFAULT
	jne .check_other_states

	; Accordingly with the byte received, decides
	; which state we should move on
.default_state:
	cmp al, MSG_READ_MEM      ; Read memory
	je .state_start_read_memory

	cmp al, MSG_WRITE_MEM     ; Write memory
	je .state_start_write_memory

	cmp al, MSG_SINGLE_STEP   ; Single-step
	je .state_start_single_step

	cmp al, MSG_CONTINUE      ; Continue
	je .state_start_continue

	cmp al, MSG_CTRLC         ; Ctrl-C/break
	je .state_start_ctrlc

	cmp al, MSG_ADD_SW_BREAK  ; Add 'software' breakpoint
	je .state_start_add_sw_breakpoint

	cmp al, MSG_REM_SW_BREAK  ; Remove 'sw' breakpoint
	je .state_start_rem_sw_break

	cmp al, MSG_REG_WRITE
	je .state_start_reg_write ; Write into register

	cmp al, MSG_ADD_HW_WATCH  ; Add a hw watchpoint
	je .state_start_add_hw_watch

	cmp al, MSG_REM_HW_WATCH  ; Remove a hw watchpoint
	je .state_start_rem_hw_watch

	jmp read_uart             ; Unrecognized byte

	; Already inside a state, check which one
	; and acts accordingly
.check_other_states:
	cmp byte [cs:state], STATE_READ_MEM
	je .state_read_memory_params

	cmp byte [cs:state], STATE_WRITE_MEM_PARAMS
	je .state_write_memory_params

	cmp byte [cs:state], STATE_WRITE_MEM
	je .state_write_memory

	cmp byte [cs:state], STATE_SW_BREAKPOINT
	je .state_add_sw_breakpoint_params

	cmp byte [cs:state], STATE_REG_WRITE_PARAMS
	je .state_reg_write_params

	cmp byte [cs:state], STATE_HW_WATCH
	je .state_add_hw_watch_params

	jmp read_uart

	; ---------------------------------------------
	; Read memory operations
	; ---------------------------------------------

	; Define read state
	;
	; Params: address (4-bytes LE) + size (2-bytes LE)
	define_start_and_params_state \
		read_memory, STATE_READ_MEM, 6

	;
	; Read memory
	;
.state_read_memory:
	; Signal that we're dumping the memory
	mov bl, MSG_READ_MEM
	call uart_write_byte

	; Convert to SEG:OFF
	mov eax, dword [cs:read_mem_addr]
	call phys_to_seg

	; Set our seg:off accordingly
	mov ds, ax
	mov si, bx

	mov cx, word [cs:read_mem_size]
	.mem_dump:
		lodsb
		mov bl, al
		call uart_write_byte
		loop .mem_dump

	; Reset our state
	mov byte [cs:state], STATE_DEFAULT
	jmp read_uart

	; ---------------------------------------------
	; Write memory operations
	; ---------------------------------------------

	;
	; Start of state
	;
.state_start_write_memory:
	mov byte  [cs:byte_counter],  0
	mov word  [cs:read_mem_size], 0
	mov dword [cs:read_mem_addr], 0
	mov byte [cs:state], STATE_WRITE_MEM_PARAMS
	jmp read_uart

	;
	; Obtain memory address to be written
	;
.state_write_memory_params:  ; params = address (4-bytes LE) +
	; Save new byte read                size (2-bytes LE)
	movzx bx, byte [cs:byte_counter]
	mov   byte [cs:read_mem_params+bx], al
	inc   byte [cs:byte_counter]

	; Check if we read everything
	cmp byte [cs:byte_counter], 6
	jne .exit
	mov byte [cs:state], STATE_WRITE_MEM
	jmp read_uart

	.exit:
		jmp read_uart

	;
	; Write memory
	;
.state_write_memory:
	mov eax, dword [cs:read_mem_addr]
	call phys_to_seg

	; SEG:OFF
	mov ds, ax
	mov si, bx

	; Write
	mov al, byte [cs:byte_read]
	mov byte [ds:si], al

	; Increment phys address
	inc dword [cs:read_mem_addr]

	; Decrement and check
	dec word [cs:read_mem_size]
	jz .end
	jmp read_uart

.end:
	; Reset state
	mov byte [cs:state], STATE_DEFAULT

	; Send an 'OK'
	mov bl, MSG_OK
	call uart_write_byte
	jmp read_uart


	; ---------------------------------------------
	; Single-step
	; ---------------------------------------------

	;
	; Start of state/single-step (and continue) function
	; (because continue is very similar)
	;
.state_start_continue:
.state_start_single_step:
%ifndef UART_POLLING
	; Retrieve segment+off
	mov ax, [cs:saved_cs]
	mov ds, ax
	mov si, [cs:saved_eip]

	; Restore origin insn
	mov eax, dword [cs:saved_insn]
	mov dword [ds:si], eax

	; Restore CS+EIP (just to be double sure where we
	; will resume our execution)
	mov ax, ds
	mov bp, sp
	mov word [ss:bp+EIP_OFF], si ; EIP
	mov word [ss:bp+CS_OFF],  ax ; CS

	; Set the 'should_step' to 1
	mov byte [cs:should_step], 1
%endif

.check_continue:
	mov bp, sp

	; Set TF as it might be disabled from a previous
	; continue
	or word [ss:bp+EFLAGS_OFF], EFLAGS_TF

	; Check if we should disable single-step or not
	; i.e: if we are in a continue message
	cmp byte [cs:byte_read], MSG_CONTINUE
	jne .not_continue

	; Clear the 'TF' flag of our EFLAGS, and
	; everything should be fine
	and word [ss:bp+EFLAGS_OFF], ~EFLAGS_TF

.not_continue:
	; Reset our state
	mov byte [cs:state], STATE_DEFAULT

	; Enable (or not) breakpoints
	call enable_hw_breakpoints
	jmp exit_int4

	; ---------------------------------------------
	; Ctrl-C/break
	; ---------------------------------------------

	;
	; Start of state
	;
.state_start_ctrlc:
	jmp handler_int1_send ; Jump to our int1 handler as if
	                      ; we're dealing with a single-step

	; ---------------------------------------------
	; Add 'software' breakpoint
	; ---------------------------------------------

	; Note: Although this is expected to be a sw breakpoint,
	; the breakpoints used here are hardware breakpoints
	; instead. This was chosen because there would be some
	; complications with int3 and the interrupt-based approach,
	; since this approach overwrites the memory too.
	;
	; In order to make things easier, I opted to use hw
	; breakpoints =)

	; Define sw breakpoint state
	;
	; Params: phys address to break (4-bytes LE)
	define_start_and_params_state \
		add_sw_breakpoint, STATE_SW_BREAKPOINT, 4

	;
	; Software breapoint
	;
.state_add_sw_breakpoint:
	mov eax, dword [cs:read_mem_addr]
	mov DR0, eax

	; Reset state
	mov byte [cs:state], STATE_DEFAULT

	; Send an 'OK'
	mov bl, MSG_OK
	call uart_write_byte
	jmp read_uart

	; ---------------------------------------------
	; Remove 'software' breakpoint
	; ---------------------------------------------

	;
	; Start of state
	;
.state_start_rem_sw_break:
	; Since we only support 1 instruction breakpoint
	; we only remove that...
	xor eax, eax
	mov DR0, eax
	call disable_hw_breakpoints

	; Reset state
	mov byte [cs:state], STATE_DEFAULT

	; Send an 'OK'
	mov bl, MSG_OK
	call uart_write_byte
	jmp read_uart

	; ---------------------------------------------
	; Register write operations
	; ---------------------------------------------

	; Define read state
	;
	; Params: register number (1-byte LE) +
	;         register value  (4-bytes LE)
	define_start_and_params_state \
		reg_write, STATE_REG_WRITE_PARAMS, 5

	;
	; Writes the read value to the appropriate
	; register
	;
.state_reg_write:
	; Read the register number and get its stack
	; offset
	xor ax,  ax
	mov al,  [cs:first_param_byte]   ; reg number
	mov ebx, [cs:second_param_dword] ; reg value
	mov bp,  sp

	cmp al, 8
	jge .reg_is_16bit
.reg_is_32bit:
	shl al, 2
	add bp, ax
	mov dword [ss:bp], ebx
	jmp .end_reg_write
.reg_is_16bit:
	sub al, 8  ; get zero-indexed
	shl al, 1  ; get its offset
	add al, 32 ; skip the first 8 32-bit regs
	add bp, ax
	mov word [ss:bp], bx
.end_reg_write:
	; Reset state
	mov byte [cs:state], STATE_DEFAULT

	; Send an 'OK'
	mov bl, MSG_OK
	call uart_write_byte
	jmp read_uart

	; ---------------------------------------------
	; Add hardware watchpoint operations
	; ---------------------------------------------

	; Define add hw watchpoint state
	;
	; Params:
	;   1-byte LE: watchpoint kind (whether write or
	;              read/write (access))
	;   4-byte LE: watchpoint address
	;
	define_start_and_params_state \
		add_hw_watch, STATE_HW_WATCH, 5

	;
	; Adds a hardware watchpoint for the given type
	; and address
	;
.state_add_hw_watch:
	mov   eax, [cs:second_param_dword]    ; Watch address
	mov   DR2, eax
	movzx eax, byte [cs:first_param_byte] ; R/W
	shl   eax, 24  ; R/W at R/W 2
	mov   ebx, DR7
	or    ebx, eax
	; 4-byte length watch & not enable it
	or    ebx, DR7_LE2_4byte
	mov   DR7, ebx

	; Reset state
	mov byte [cs:state], STATE_DEFAULT

	; Send an 'OK'
	mov bl, MSG_OK
	call uart_write_byte
	jmp read_uart

	; ---------------------------------------------
	; Remove hardware watchpoint operations
	; ---------------------------------------------

	;
	; Start of state
	;
.state_start_rem_hw_watch:
	; Since we only support 1 watchpoint, there is
	; no need to do fancy stuff
	xor eax, eax
	mov DR2, eax
	mov eax, DR7
	and eax, ~(DR7_L2 | DR7_LE2_RW | DR7_LE2_4byte)
	mov DR7, eax

	; Reset state
	mov byte [cs:state], STATE_DEFAULT

	; Send an 'OK'
	mov bl, MSG_OK
	call uart_write_byte
	jmp read_uart

exit_int4:
	pop_regs
	iret

%ifndef UART_POLLING
;
; Configure PIC master & slave
;
setup_pic:
	; Starts initialization sequence
	outbyte PIC1_COMMAND, 0x11
	iowait
	outbyte PIC2_COMMAND, 0x11
	iowait
	; Set the same vector again
	outbyte PIC1_DATA, 0x20
	iowait
	outbyte PIC2_DATA, 0x28
	iowait
	; Tell the master that there is a slave
	; and the slave that there is a master
	outbyte PIC1_DATA, 0x04
	iowait
	outbyte PIC2_DATA, 0x02
	iowait
	; Set 8086 mode. */
	outbyte PIC1_DATA, 0x01
	iowait
	outbyte PIC2_DATA, 0x01
	iowait
	; Clear interrupt mask
	outbyte PIC1_DATA, 0xEF
	ret
%endif

;
; Configure UART
;
; 8 bits, no parity, one stop bit
;
setup_uart:
	; Calculate and set divisor
	outbyte UART_LCR,  UART_LCR_DLA
	outbyte UART_DLB1, UART_DIVISOR & 0xFF
	outbyte UART_DLB2, UART_DIVISOR >> 8
	; Set line control register:
	outbyte UART_LCR, UART_LCR_BPC_8
	; Reset FIFOs and set trigger level to 1 byte.
	outbyte UART_FCR, UART_FCR_CLRRECV|UART_FCR_CLRTMIT|UART_FCR_TRIG_1
	; IRQs enabled, RTS/DSR set
	outbyte UART_MCR, UART_MCR_OUT2|UART_MCR_RTS|UART_MCR_DTR
	; Enable 'Data Available Interrupt'
%ifndef UART_POLLING
	outbyte UART_IER, 1
%endif
	ret

;
; Write a string to UART
; Parameters:
;   si = String offset
;
uart_write_string:
	.loop:
		lodsb
		cmp al, 0
		je .out
		mov bl, al
		call uart_write_byte
		jmp .loop
	.out:
		ret

;
; Write a single byte to UART
; Parameters:
;   bl = byte to be sent
;
uart_write_byte:
	mov dx, UART_LSR
	.loop:
		in  al, dx
		and al, UART_LSR_TFE
		cmp al, 0
		je .loop
	mov dx, UART_BASE
	mov al, bl
	out dx, al
	ret

;
; Write a word to UART
;
; Parameters:
;   bx = word to be written
;
uart_write_word:
	call uart_write_byte
	shr bx, 8
	call uart_write_byte
	ret

;
; Write a dword to UART
;
; Parameters:
;   ebx = dword to be written
;
uart_write_dword:
	call uart_write_byte
	shr ebx, 8
	call uart_write_byte
	shr ebx, 8
	call uart_write_byte
	shr ebx, 8
	call uart_write_byte
	ret

;
; Convert a physical address to SEG:OFF
; Parameters:
;   eax = Physical address
; Return:
;   ax = Segment
;   bx = Offset
;
phys_to_seg:
	cmp eax, (1<<20)
	jge .a20_addr
	mov bx, ax
	shr eax, 4
	and eax, 0xF000
	ret
.a20_addr:
	mov ebx, eax
	mov ax,  0xFFFF
	sub ebx, 0xFFFF0
	ret

;
; Send all regs and stop reason over UART to the bridge
;
; Note: This function assumes that the stack
; follows the layout that 'push_regs' leave,
; ie:
;   Stack:
;     ret_addr/eip (from this function)
;     push_regs
;
send_stop_msg:
	; Signal that we stopped!
	mov bl, MSG_SINGLE_STEP
	call uart_write_byte

	;
	; Now we need to send our registers
	; First: 32-bit regs: EDI-EAX
	;
	mov ax, ss
	mov ds, ax
	mov si, sp
	add si, 2   ; Ignores return address/EIP
	mov cx, 8
	.loop1:
		lodsd
		mov ebx, eax
		call uart_write_dword
		loop .loop1

	; Second: 16-bit regs: GS-EFLAGS
	mov cx, 8
	.loop2:
		lodsw
		mov bx, ax
		call uart_write_word
		loop .loop2

	; Discover what make us stop and send the reason
	mov eax, DR6
	bt  ax,  2
	jnc .normal_break

	; Watchpoint break in DR2
	mov bl, STOP_REASON_WATCHPOINT
	call uart_write_byte
	mov eax, DR2
	mov ebx, eax
	call uart_write_dword
	ret

.normal_break:
	mov bl, STOP_REASON_NORMAL
	call uart_write_byte
	call uart_write_dword ; stub value, should not be used
	ret

;
; Disable breakpoints and reset DR6
; Parameters:
;   none
;
disable_hw_breakpoints:
	; Reset DR6
	mov eax, DR6
	xor al,  al   ; Clear L0/G0-L3/G3
	mov DR6, eax
	; Disable LE0 and LE2
	mov eax, DR7
	and al, ~(DR7_L0 | DR7_L2)
	mov DR7, eax
	ret

;
; Enable breakpoints and reset DR6
; Parameters:
;   none
;
; Note: This function assumes that the stack
; follows the layout that 'push_regs' leave,
; ie:
;   Stack:
;     ret_addr/eip (from this function)
;     push_regs
;
enable_hw_breakpoints:
	; Reset DR6
	mov eax, DR6
	and al,  0xF0
	mov DR6, eax

	; Enable insn hw breakpoints
	call enable_insn_hw_bp

	; Enable watchpoints if any
	mov eax, DR2
	cmp eax, 0
	je  .not_enable
	mov eax, DR7
	or  eax, DR7_L2 ; We can always safely enable
	mov DR7, eax    ; when returning to execute

.not_enable:
	ret

;
; Enable instruction breakpoints if any
;
; Parameters:
;   none
;
; Required stack layout:
; Stack:
;   ret_addr
;   ret_addr
;   push_regs
;
enable_insn_hw_bp:
	; Get phys addr
	mov   bp,  sp
	add   bp,  4
	movzx eax, word [ss:bp+CS_OFF]
	shl   eax, 4
	movzx ebx, word [ss:bp+EIP_OFF]
	add   eax, ebx ; Current physical address

	; Check if there is something First
	mov ebx, DR0
	cmp ebx, 0
	mov ecx, DR7
	jz  .not_enable

	;
	; Compare current addr with break addr
	; if the same, if shouldn't enable it!
	; otherwise, thats no problem =)
	;
	cmp eax, ebx
	jne .enable
.not_enable:
	and ecx, ~DR7_L0 ; Disabled
	jmp .exit
.enable:
	or  ecx,  DR7_L0 ; Enabled
.exit:
	mov DR7, ecx
	ret

; --------------------------------
; Data
; --------------------------------
idtptr:
	idt_limit: dw 0
	idt_base:  dd 0

%ifndef UART_POLLING
should_step:
	db 1
saved_cs:
	dw 0
saved_eip:
	dw 0
saved_insn:
	dd 0
%endif


; State machine
state:
	db STATE_DEFAULT
byte_read:     ; Last byte read via UART
	db 0

; Read memory data
byte_counter:  ; Byte counter in a sequence of bytes read
	db 0       ; within a given state

read_mem_params:
read_mem_addr:
first_param_byte:
	db 0
second_param_dword:
	db 0,0,0
read_mem_size:
	db 0,0

; --------------------------------
; Strings
; --------------------------------
str_idt_gt1MB_error: db 'IDT >= 1MB', 0
