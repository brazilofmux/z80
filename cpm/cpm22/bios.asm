; bios.asm — the z80-monster CP/M 2.2 BIOS.
;
; A real BIOS in the CP/M sense: the 17-entry jump table the BDOS and
; CCP expect at the top of memory, cold and warm boot, the disk parameter
; headers and blocks. What it drives is not a floppy controller but the
; host, through a dozen I/O ports (0E0h-0EFh; see cpm/cpm_host.c, which
; is the other half of this file). Every port access traps to the
; interpreter; everything else here is ordinary Z80 code the JIT
; translates like any other.
;
; Memory map (62K system, the layout the shim already used):
;   DC00  CCP    (2048 bytes, from ccp.asm)
;   E400  BDOS   (3584 bytes, from bdos.asm; entry E406)
;   F200  BIOS   (this file)
;
; Assembles with Macro Assembler AS: see the Makefile target `system`.

	cpu	z80

ccp	equ	0DC00h
bdos	equ	0E400h
bios	equ	0F200h

iobyte	equ	0003h		; I/O byte
cdisk	equ	0004h		; current disk (low nibble) / user (high nibble)
sysend	equ	bdos+0E00h	; first byte past the BDOS = bios

; ---- host ports (0E0h-0EFh; clear of every real Kaypro port) ----
p_const	equ	0E0h		; IN : 0 / 0FFh
p_con	equ	0E1h		; IN : CONIN (blocks) / OUT: CONOUT
p_list	equ	0E2h		; OUT: LIST
p_aux	equ	0E3h		; IN : READER / OUT: PUNCH
p_sys	equ	0E5h		; OUT: 0 = reload CCP+BDOS, 1 = exit emulator
p_dbg	equ	0E7h		; OUT: debug byte (shown under -d)
p_drive	equ	0E8h		; OUT: select drive 0-15 / IN: type 0 none, 1 fd, 2 hd
p_trkl	equ	0E9h		; OUT: track low
p_trkh	equ	0EAh		; OUT: track high
p_secl	equ	0EBh		; OUT: sector low  (0-based, 128-byte records)
p_sech	equ	0ECh		; OUT: sector high
p_dmal	equ	0EDh		; OUT: DMA low
p_dmah	equ	0EEh		; OUT: DMA high
p_cmd	equ	0EFh		; OUT: 0 read, 1 write / IN: 0 ok, 1 error

ndrives	equ	8		; A: to H: (a batch job wants work and scratch drives)

	org	bios

; ---- the jump table ----
	jp	boot
wboote:	jp	wboot
	jp	const
	jp	conin
	jp	conout
	jp	list
	jp	punch
	jp	reader
	jp	home
	jp	seldsk
	jp	settrk
	jp	setsec
	jp	setdma
	jp	read
	jp	write
	jp	listst
	jp	sectran

; ---- boot ----
boot:	ld	sp,80h
	ld	hl,signon
	call	puts
	xor	a
	ld	(iobyte),a
	ld	(cdisk),a
	jr	gocpm

; Warm boot: a program may have overwritten the CCP (and the BDOS), so
; ask the host to put the system image back, then re-enter the CCP with
; an empty command line. (A floppy BIOS re-reads the system tracks here.)
wboot:	ld	sp,80h
	xor	a
	out	(p_sys),a
gocpm:	ld	a,0C3h		; JP
	ld	(0000h),a
	ld	hl,wboote
	ld	(0001h),hl
	ld	(0005h),a
	ld	hl,bdos+6
	ld	(0006h),hl
	ld	bc,80h
	call	setdma
	ld	a,(cdisk)
	ld	c,a
	jp	ccp

puts:	ld	a,(hl)
	or	a
	ret	z
	ld	c,a
	push	hl
	call	conout
	pop	hl
	inc	hl
	jr	puts

signon:	db	0Dh,0Ah,"62K CP/M 2.2 on the z80-monster",0Dh,0Ah,0

; ---- console / list / punch / reader ----
const:	in	a,(p_const)
	ret
conin:	in	a,(p_con)
	ret
conout:	ld	a,c
	out	(p_con),a
	ret
list:	ld	a,c
	out	(p_list),a
	ret
punch:	ld	a,c
	out	(p_aux),a
	ret
reader:	in	a,(p_aux)
	ret
listst:	ld	a,0FFh		; the host printer is always ready
	ret

; ---- disk ----
home:	ld	bc,0
	; fall into settrk
settrk:	ld	a,c
	out	(p_trkl),a
	ld	a,b
	out	(p_trkh),a
	ret
setsec:	ld	a,c
	out	(p_secl),a
	ld	a,b
	out	(p_sech),a
	ret
setdma:	ld	a,c
	out	(p_dmal),a
	ld	a,b
	out	(p_dmah),a
	ret
read:	xor	a
	out	(p_cmd),a
	in	a,(p_cmd)
	ret
write:	ld	a,1
	out	(p_cmd),a
	in	a,(p_cmd)
	ret
sectran:			; no skew: physical = logical
	ld	h,b
	ld	l,c
	ret

; SELDSK: C = drive. Returns HL = DPH, or 0 if the host has no image
; mounted there. The DPH's DPB pointer is set to the block matching the
; image the host reports (floppy or hard disk), so a drive can hold
; either kind.
seldsk:	ld	hl,0
	ld	a,c
	cp	ndrives
	ret	nc
	out	(p_drive),a
	in	a,(p_drive)
	or	a
	ret	z		; nothing mounted
	ld	de,dpb_fd
	dec	a
	jr	z,seldsk1
	ld	de,dpb_hd
seldsk1:
	ld	a,c		; HL = dph + 16*C
	add	a,a
	add	a,a
	add	a,a
	add	a,a
	ld	l,a
	ld	h,0
	ld	bc,dph
	add	hl,bc
	push	hl
	ld	bc,10		; DPB pointer lives at DPH+10
	add	hl,bc
	ld	(hl),e
	inc	hl
	ld	(hl),d
	pop	hl
	ret

; ---- disk parameter headers, one per drive ----
; XLT (none), 6 scratch bytes, DIRBUF, DPB (patched by seldsk), CSV, ALV.
; CKS is 0 for both formats (the images never change under a running
; system), so CSV needs no space; ALV is sized for the larger format.
dphm	macro	n
	dw	0, 0, 0, 0
	dw	dirbuf, dpb_hd, 0, alv + n*alvsz
	endm
dph:	dphm	0
	dphm	1
	dphm	2
	dphm	3
	dphm	4
	dphm	5
	dphm	6
	dphm	7

; 8 MB "hard disk": 1024 tracks x 64 records, 4K blocks, 1024 directory
; entries, 2 system tracks. cpmtools diskdef z80m-hd in tools/diskdefs.
dpb_hd:	dw	64		; SPT  records per track
	db	5, 31, 1	; BSH BLM EXM (4K blocks, >255 blocks)
	dw	2043		; DSM  last block number
	dw	1023		; DRM  last directory entry
	db	0FFh, 0		; AL0 AL1: 8 directory blocks
	dw	0		; CKS  fixed media
	dw	2		; OFF  system tracks

; 400K "floppy": 80 tracks x 40 records, 2K blocks, 64 entries, 2 system
; tracks. cpmtools diskdef z80m-fd.
dpb_fd:	dw	40
	db	4, 15, 1	; 2K blocks, <256 blocks: EXM 1
	dw	194
	dw	63
	db	80h, 0		; 1 directory block
	dw	0
	dw	2

; The uninitialised areas come last: the system image is cut at
; the end of the initialised code and data (Makefile: f200-f9ff).
dirbuf:	ds	128
alvsz	equ	256		; (DSM/8)+1 for the hard disk
alv:	ds	ndrives*alvsz

biosend:
	end
