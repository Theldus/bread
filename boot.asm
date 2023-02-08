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
[ORG 0x7C00]

boot_init:
	cli
	mov ax, 0x7C0
	add ax, 544
	mov ss, ax
	mov sp, 4096

	; Read 10 sectors
	mov   ax, 0x1000 ; segment
	mov   es, ax
	mov   bx, 0x0000 ; offset addr
	mov   al, 10     ; num sectors to read
	mov   cl, 2      ; from sector 2 (1-based)
	call  read_sectors

	; Far call to our module
	call 0x1000:0x0000

	; Hang
	jmp $

read_sectors:
	mov ah, 2
	mov ch, 0
	mov dh, 0
	int 0x13
	jc  .again
	ret
.again:
	xor ax, ax
	int 0x13
	jmp read_sectors
	ret

times 510-($-$$) db 0
dw 0xAA55
