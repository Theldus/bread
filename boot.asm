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
