; sort.asm - an external sort for CP/M 2.2 in Z80 assembly: the SORT
; step of a batch job, the kind CP/M never shipped.
;
; SORT.COM asks three console lines (a job stream can type them):
;     SORTIN file, SORTOUT file, SYSIN deck
; and reads a DFSORT-style deck
;     * comment
;     SORT FIELDS=(pos,len,CH,A,pos,len,CI,D,...)   may span lines
; Positions are 1-based; CH compares bytes, CI folds a-z to A-Z; A/D
; ascending/descending; a field past the end of a record reads as
; spaces, so 1,256,CH,A is the whole record.  Stable: equal keys keep
; their input order.  Records are text lines (CR LF, control-Z ends
; the file) of up to 255 bytes.
;
; Plan: the deck is parsed once into a key table; the memory between
; the program and the BDOS is then a pool.  Records fill the pool as
; [len][bytes]; a pointer table at the top is kept sorted by binary
; insertion (upper bound, for stability) as each record arrives.  A
; full pool is a run, written to B:Rnnn.TMP.  Runs are merged 16 at a
; time, levels alternating B: and C:, consumed runs erased, the last
; level into SORTOUT.  A single run goes straight to SORTOUT.
;
; Assembles with Macro Assembler AS (make cpm/sort/SORT.COM); the two
; work drives B: and C: must be mounted. tests/sort.sh checks it
; against tests/sort/sortref.py.

	cpu	z80
	org	100h

bdos	equ	5
b_conout equ	2
b_print	equ	9
b_rdline equ	10
b_open	equ	15
b_close	equ	16
b_erase	equ	19
b_read	equ	20
b_write	equ	21
b_make	equ	22
b_setdma equ	26

maxk	equ	16		; merge fan-in
maxkeys	equ	8
maxrec	equ	255
ptrmax	equ	4096		; records per run at most (8K of pointers)

; input stream: pos 1, cnt 1, eof 1, fcb 36, buf 128, head 256 (the
; byte fields first: an IX displacement only reaches 127)
i_pos	equ	0
i_cnt	equ	1
i_eof	equ	2
i_fcb	equ	3
i_buf	equ	39
i_head	equ	167
i_size	equ	167+256

; output stream: pos 1, pad 2, fcb 36, buf 128
o_pos	equ	0
o_fcb	equ	3
o_buf	equ	39
o_size	equ	167

; key entry: pos 2, len 2, flags 1 (bit0 descending, bit1 fold)
k_pos	equ	0
k_len	equ	2
k_flag	equ	4
k_size	equ	5

; ---------------------------------------------------------------
start:	ld	sp,stack
	ld	de,m_in
	ld	hl,inname
	call	askline
	ld	de,m_out
	ld	hl,outname
	call	askline
	ld	de,m_deck
	ld	hl,deckname
	call	askline
	call	readdeck
	call	parsedeck
	ld	a,(nkeys)
	or	a
	jp	z,nokeys
	call	setpool
	call	formruns
	ld	de,m_runs
	call	puts
	ld	hl,(nruns)
	call	putdec
	ld	de,m_formed
	call	puts
	call	mergeruns
	ld	de,m_done
	call	puts
	ld	hl,(total)
	call	putdec
	ld	de,m_recs
	call	puts
	jp	0

nokeys:	ld	de,m_nokeys
	call	puts
	jp	0

; ---- console --------------------------------------------------
; askline: prompt DE, read a line into (HL) as an upper-case name,
; NUL-terminated, at most 30 chars.
askline: push	hl
	call	puts
	ld	a,30
	ld	(conbuf),a
	ld	de,conbuf
	ld	c,b_rdline
	call	bdos
	ld	e,13
	ld	c,b_conout
	call	bdos
	ld	e,10
	ld	c,b_conout
	call	bdos
	pop	de
	ld	hl,conbuf+2
	ld	a,(conbuf+1)
	ld	b,a
	or	a
	jr	z,askz
askc:	ld	a,(hl)
	cp	'a'
	jr	c,askn
	cp	'z'+1
	jr	nc,askn
	and	0DFh
askn:	ld	(de),a
	inc	hl
	inc	de
	djnz	askc
askz:	xor	a
	ld	(de),a
	ret

puts:	ld	c,b_print
	jp	bdos

; putdec: HL as decimal, no leading zeros (div16 uses DE and BC)
putdec:	ld	de,dbuf+5
	xor	a
	ld	(de),a
pd1:	dec	de
	push	de
	ld	bc,10
	call	div16		; HL = HL/10, A = remainder
	pop	de
	add	a,'0'
	ld	(de),a
	ld	a,h
	or	l
	jr	nz,pd1
	ex	de,hl
pd3:	ld	a,(hl)
	or	a
	ret	z
	push	hl
	ld	e,a
	ld	c,b_conout
	call	bdos
	pop	hl
	inc	hl
	jr	pd3

; div16: HL = HL / BC, A = HL mod BC (BC small)
div16:	ld	de,0
	ld	a,16
div1:	add	hl,hl
	rl	e
	rl	d
	push	hl
	ld	h,d
	ld	l,e
	or	a
	sbc	hl,bc
	jr	c,div2
	ld	d,h
	ld	e,l
	pop	hl
	inc	l
	jr	div3
div2:	pop	hl
div3:	dec	a
	jr	nz,div1
	ld	a,e
	ret

; ---- FCB from a name ------------------------------------------
; HL -> NUL-terminated name (upper case), DE -> 36-byte FCB.
; Keeps HL and DE (callers parse the same name more than once).
parsefcb: push	hl
	push	de
	push	bc
	call	parsefcb0
	pop	bc
	pop	de
	pop	hl
	ret
parsefcb0: push	hl
	push	de
	ld	h,d
	ld	l,e
	ld	(hl),0
	ld	d,h
	ld	e,l
	inc	de
	ld	bc,35
	ldir
	pop	de
	pop	hl
	push	de
	inc	hl
	ld	a,(hl)
	dec	hl
	cp	':'
	jr	nz,pf1
	ld	a,(hl)
	sub	'A'-1
	ld	(de),a
	inc	hl
	inc	hl
pf1:	inc	de
	ld	b,8
	call	pfpart
	ld	a,(hl)
	cp	'.'
	jr	nz,pf2
	inc	hl
pf2:	ld	b,3
	call	pfpart
	pop	de
	ret
; copy up to B chars of name to (DE), stop at '.' or NUL, pad spaces
pfpart:	ld	a,(hl)
	or	a
	jr	z,pfpad
	cp	'.'
	jr	z,pfpad
	ld	(de),a
	inc	hl
	inc	de
	djnz	pfpart
pfskip:	ld	a,(hl)		; name part too long: skip the rest
	or	a
	ret	z
	cp	'.'
	ret	z
	inc	hl
	jr	pfskip
pfpad:	ld	a,' '
	ld	(de),a
	inc	de
	djnz	pfpad
	ret

; ---- input streams (IX -> stream) ----------------------------
; fcbde: DE -> the stream's FCB
fcbde:	push	hl
	push	ix
	pop	hl
	ld	de,i_fcb
	add	hl,de
	ex	de,hl
	pop	hl
	ret
; in_open: HL -> name.  Aborts on failure.
in_open: call	fcbde
	call	parsefcb
	call	fcbde
	ld	c,b_open
	call	bdos
	cp	0FFh
	jr	z,openfail
	xor	a
	ld	(ix+i_pos),a
	ld	(ix+i_cnt),a
	ld	(ix+i_eof),a
	ret
openfail: ld	de,m_noopen
	call	puts
	jp	0

in_close: call	fcbde
	ld	c,b_close
	jp	bdos

; in_getc: A = next byte; carry set at end of file.  Keeps BC, DE, HL
; (in_getline holds its state in them); the refill's BDOS calls do not.
in_getc: push	bc
	push	de
	push	hl
	call	in_getc0
	pop	hl
	pop	de
	pop	bc
	ret
in_getc0: ld	a,(ix+i_eof)
	or	a
	jr	nz,ingeof
	ld	a,(ix+i_pos)
	cp	(ix+i_cnt)
	jr	c,ingok
	; refill
	push	ix
	pop	de
	ld	hl,i_buf
	add	hl,de
	ex	de,hl
	ld	c,b_setdma
	call	bdos
	call	fcbde
	ld	c,b_read
	call	bdos
	or	a
	jr	nz,ingeof2
	xor	a
	ld	(ix+i_pos),a
	ld	a,128
	ld	(ix+i_cnt),a
	xor	a
ingok:	push	ix
	pop	hl
	ld	de,i_buf
	add	hl,de
	ld	e,a
	ld	d,0
	add	hl,de
	inc	(ix+i_pos)
	ld	a,(hl)
	cp	1Ah
	jr	z,ingeof2
	or	a		; clear carry
	ret
ingeof2: ld	a,1
	ld	(ix+i_eof),a
ingeof:	scf
	ret

; in_getline: read a line into (DE) as [len][bytes]; carry set at EOF
; with nothing read.
in_getline: push	de
	inc	de
	ld	b,0		; length
igl1:	call	in_getc
	jr	c,igl9
	cp	13
	jr	z,igl1
	cp	10
	jr	z,igl8
	ld	c,a
	ld	a,b
	cp	maxrec
	jr	nc,igl1		; too long: drop the rest
	ld	a,c
	ld	(de),a
	inc	de
	inc	b
	jr	igl1
igl9:	ld	a,b		; EOF: a last line without LF still counts
	or	a
	jr	nz,igl8
	pop	de
	scf
	ret
igl8:	pop	de
	ld	a,b
	ld	(de),a
	or	a
	ret

; ---- output stream (IX -> stream) -----------------------------
out_open: push	hl		; the name: the BDOS call spoils HL
	call	fcbde
	call	parsefcb
	call	fcbde
	ld	c,b_erase
	call	bdos
	pop	hl
	call	fcbde
	call	parsefcb	; erase spoils the FCB; rebuild it
	call	fcbde
	ld	c,b_make
	call	bdos
	cp	0FFh
	jp	z,openfail
	xor	a
	ld	(ix+o_pos),a
	ret

; out_putc: A -> stream
out_putc: push	hl
	push	de
	push	bc
	push	af
	ld	a,(ix+o_pos)
	cp	128
	call	nc,out_flush
	push	ix
	pop	hl
	ld	de,o_buf
	add	hl,de
	ld	e,(ix+o_pos)
	ld	d,0
	add	hl,de
	pop	af
	ld	(hl),a
	inc	(ix+o_pos)
	pop	bc
	pop	de
	pop	hl
	ret

out_flush: push	af
	push	ix
	pop	de
	ld	hl,o_buf
	add	hl,de
	ex	de,hl
	ld	c,b_setdma
	call	bdos
	call	fcbde
	ld	c,b_write
	call	bdos
	or	a
	jr	nz,writefail
	xor	a
	ld	(ix+o_pos),a
	pop	af
	ret
writefail: ld	de,m_full
	call	puts
	jp	0

; out_putrec: HL -> [len][bytes]; writes bytes + CR LF
out_putrec: ld	b,(hl)
	inc	hl
	ld	a,b
	or	a
	jr	z,opr2
opr1:	ld	a,(hl)
	call	out_putc
	inc	hl
	djnz	opr1
opr2:	ld	a,13
	call	out_putc
	ld	a,10
	jp	out_putc

out_close: ld	a,1Ah
	call	out_putc
	ld	a,(ix+o_pos)
	or	a
	jr	z,oc2
oc1:	ld	a,1Ah
	call	out_putc
	ld	a,(ix+o_pos)
	cp	128
	jr	c,oc1
	call	out_flush
oc2:	call	fcbde
	ld	c,b_close
	jp	bdos

; ---- the deck --------------------------------------------------
readdeck: ld	ix,rin
	ld	hl,deckname
	call	in_open
	ld	hl,deckbuf
	ld	bc,deckmax
rd1:	push	hl
	push	bc
	call	in_getc
	pop	bc
	pop	hl
	jr	c,rd2
	ld	(hl),a
	inc	hl
	dec	bc
	ld	a,b
	or	c
	jr	nz,rd1
rd2:	xor	a
	ld	(hl),a
	ld	ix,rin
	jp	in_close

; parsedeck: find "(" outside comment lines, then pos,len,fmt,order
; groups up to ")".
parsedeck: ld	hl,deckbuf
	xor	a
	ld	(nkeys),a
pdl:	ld	a,(hl)		; at a line start
	or	a
	ret	z
	cp	'*'
	jr	nz,pds
pdskip:	inc	hl		; skip the comment line
	ld	a,(hl)
	or	a
	ret	z
	cp	10
	jr	nz,pdskip
	inc	hl
	jr	pdl
pds:	ld	a,(hl)
	or	a
	ret	z
	cp	10
	jr	z,pdnl
	cp	'('
	jr	z,pdkeys
	inc	hl
	jr	pds
pdnl:	inc	hl
	jr	pdl
pdkeys:	inc	hl
	ld	iy,keys
pdk1:	call	pdnum		; position
	ld	(iy+k_pos),e
	ld	(iy+k_pos+1),d
	call	pdsep
	call	pdnum		; length
	ld	(iy+k_len),e
	ld	(iy+k_len+1),d
	call	pdsep
	ld	(iy+k_flag),0
	call	pdws		; format: CH or CI (anything else as CH)
	ld	a,(hl)
	inc	hl
	call	pdws
	ld	a,(hl)
	cp	'I'
	jr	nz,pdk2
	set	1,(iy+k_flag)
pdk2:	inc	hl
	call	pdsep
	call	pdws
	ld	a,(hl)		; order
	inc	hl
	cp	'D'
	jr	nz,pdk3
	set	0,(iy+k_flag)
pdk3:	ld	a,(nkeys)
	inc	a
	ld	(nkeys),a
	call	pdws
	ld	a,(hl)
	cp	')'
	ret	z
	or	a
	ret	z
	cp	','
	ret	nz
	inc	hl
	ld	a,(nkeys)
	cp	maxkeys
	ret	nc
	ld	de,k_size
	add	iy,de
	jr	pdk1
; skip blanks, CR, LF
pdws:	ld	a,(hl)
	cp	' '
	jr	z,pdws1
	cp	13
	jr	z,pdws1
	cp	10
	ret	nz
pdws1:	inc	hl
	jr	pdws
; skip blanks then a comma
pdsep:	call	pdws
	ld	a,(hl)
	cp	','
	ret	nz
	inc	hl
	ret
; decimal number -> DE
pdnum:	call	pdws
	ld	de,0
pdn1:	ld	a,(hl)
	sub	'0'
	ret	c
	cp	10
	ret	nc
	push	hl
	ld	h,d
	ld	l,e
	add	hl,hl
	add	hl,hl
	add	hl,de
	add	hl,hl
	ld	e,a
	ld	d,0
	add	hl,de
	ex	de,hl
	pop	hl
	inc	hl
	jr	pdn1

; ---- compare ----------------------------------------------------
; cmprec: HL -> record A, DE -> record B ([len][bytes]).
; Returns: Z equal; else C if A < B, NC if A > B.
cmprec:	ld	(cra),hl
	ld	(crb),de
	ld	iy,keys
	ld	a,(nkeys)
	ld	b,a
crk:	push	bc
	ld	e,(iy+k_pos)
	ld	d,(iy+k_pos+1)
	dec	de		; 0-based offset
	ld	c,(iy+k_len)
	ld	b,(iy+k_len+1)
crb1:	ld	a,b
	or	c
	jr	z,crkeq		; key exhausted: equal on this key
	push	bc
	push	de
	ld	hl,(cra)
	call	crbyte		; A = byte of record A at DE, or space
	ld	(bytea),a
	ld	hl,(crb)
	call	crbyte
	ld	(byteb),a
	pop	de
	inc	de
	pop	bc
	ld	a,(bytea)
	ld	l,a
	ld	a,(byteb)
	ld	h,a		; L = byte of A, H = byte of B
	ld	a,l
	cp	h
	jr	z,crnext
	bit	1,(iy+k_flag)	; fold?
	jr	z,crdiff
	ld	a,l
	call	fold
	ld	l,a
	ld	a,h
	call	fold
	ld	h,a
	ld	a,l
	cp	h
	jr	z,crnext
crdiff:	; the bytes differ; compare last, after the flag test (BIT
	; would spoil the Z flag the caller reads)
	bit	0,(iy+k_flag)
	jr	z,crasc
	ld	a,h		; descending: the other way round
	cp	l
	jr	crres
crasc:	ld	a,l
	cp	h
crres:	pop	bc
	ret			; NZ, and C if record A sorts first
crnext:	dec	bc
	jr	crb1
crkeq:	pop	bc
	push	de
	ld	de,k_size
	add	iy,de
	pop	de
	djnz	crk
	xor	a		; all keys equal: Z, NC
	ret
; crbyte: HL -> record, DE = offset; A = byte or ' ' past the end
crbyte:	ld	a,d
	or	a
	jr	nz,crsp
	ld	a,e
	cp	(hl)
	jr	nc,crsp
	inc	hl
	add	hl,de
	ld	a,(hl)
	ret
crsp:	ld	a,' '
	ret
fold:	cp	'a'
	ret	c
	cp	'z'+1
	ret	nc
	and	0DFh
	ret

; ---- the pool -------------------------------------------------
; setpool: pool from poolbase up to the pointer table under the BDOS.
setpool: ld	hl,(6)		; BDOS entry: its page is the ceiling
	ld	l,0
	ld	de,ptrmax*2
	or	a
	sbc	hl,de
	ld	(ptrs),hl	; pointer table base (ascending)
	ld	de,256		; room for the stack? no: the stack is
	or	a		; below the program data; this is margin
	sbc	hl,de
	ld	(poolend),hl
	ret

; formruns: read SORTIN into runs.  A pending record (read but not
; fitting) carries over to the next run.
formruns: ld	ix,rin
	ld	hl,inname
	call	in_open
	xor	a
	ld	(nruns),a
	ld	(nruns+1),a
	ld	(pending),a
	ld	hl,0
	ld	(total),hl
fr1:	; start a run
	ld	hl,0
	ld	(count),hl
	ld	hl,poolbase
	ld	(poolnext),hl
fr2:	; next record: pending or read
	ld	a,(pending)
	or	a
	jr	nz,fr3
	ld	ix,rin
	ld	de,line
	call	in_getline
	jr	c,fr5		; EOF
fr3:	xor	a
	ld	(pending),a
	; fits?  poolnext + len + 1 <= ptrs (the table has its own room)
	ld	hl,(poolnext)
	ld	a,(line)
	ld	e,a
	ld	d,0
	add	hl,de
	inc	hl
	ld	(newnext),hl
	ld	de,(ptrs)
	or	a
	sbc	hl,de
	jr	nc,fr4		; would overflow: run is full
	ld	hl,(count)
	ld	de,ptrmax
	or	a
	sbc	hl,de
	jr	nc,fr4
	; copy the line into the pool and insert its pointer
	ld	hl,line
	ld	de,(poolnext)
	push	de
	ld	a,(line)
	ld	c,a
	ld	b,0
	inc	bc
	ldir
	pop	de
	ld	hl,(newnext)
	ld	(poolnext),hl
	call	insertptr	; DE -> record
	ld	hl,(count)
	inc	hl
	ld	(count),hl
	ld	hl,(total)
	inc	hl
	ld	(total),hl
	jr	fr2
fr4:	ld	a,1
	ld	(pending),a
	call	writerun
	jr	fr1
fr5:	; input exhausted: last run (or the answer)
	ld	ix,rin
	call	in_close
	ld	hl,(count)
	ld	a,h
	or	l
	jr	nz,fr6
	ld	hl,(nruns)
	ld	a,h
	or	l
	ret	nz
	; no records at all: empty SORTOUT
	ld	ix,rout
	ld	hl,outname
	call	out_open
	ld	ix,rout
	jp	out_close
fr6:	ld	hl,(nruns)
	ld	a,h
	or	l
	jr	nz,fr7
	; one run is the answer
	ld	hl,outname
	call	writeto
	ld	hl,1
	ld	(nruns),hl
	ret
fr7:	jp	writerun

; insertptr: DE -> new record; binary search ptrs[0..count) for the
; first entry greater than it (upper bound), shift, store.
insertptr: ld	(newrec),de
	ld	hl,0
	ld	(lo),hl
	ld	hl,(count)
	ld	(hi),hl		; search [lo,hi)
ip1:	ld	hl,(lo)
	ld	de,(hi)
	or	a
	sbc	hl,de
	jr	nc,ip4		; lo >= hi: insert at lo
	ld	hl,(lo)
	add	hl,de
	srl	h
	rr	l
	ld	(mid),hl
	add	hl,hl
	ld	de,(ptrs)
	add	hl,de
	ld	e,(hl)
	inc	hl
	ld	d,(hl)		; DE -> ptrs[mid]
	ld	hl,(newrec)
	ex	de,hl		; HL -> ptrs[mid], DE -> new
	call	cmprec		; ptrs[mid] vs new
	jr	z,ip2
	jr	nc,ip3		; ptrs[mid] > new: hi = mid
ip2:	ld	hl,(mid)	; <= : lo = mid + 1
	inc	hl
	ld	(lo),hl
	jr	ip1
ip3:	ld	hl,(mid)
	ld	(hi),hl
	jr	ip1
ip4:	; shift ptrs[lo..count) up by one entry, from the top down
	ld	hl,(count)
	ld	de,(lo)
	or	a
	sbc	hl,de		; BC = entries to move
	ld	b,h
	ld	c,l
	ld	hl,(count)
	add	hl,hl
	ld	de,(ptrs)
	add	hl,de		; HL -> ptrs[count] (one past the end)
	ld	a,b
	or	c
	jr	z,ip6
	ld	d,h
	ld	e,l
	inc	de		; DE = last byte of the new position
	dec	hl		; HL = last byte of the old position
	sla	c
	rl	b		; bytes = entries * 2
	lddr
ip6:	ld	hl,(lo)
	add	hl,hl
	ld	de,(ptrs)
	add	hl,de
	ld	de,(newrec)
	ld	(hl),e
	inc	hl
	ld	(hl),d
	ret

; writerun: the pool in pointer order to B:Rnnn.TMP, nruns++
writerun: ld	hl,(nruns)
	inc	hl
	ld	(nruns),hl
	ld	a,'B'
	ld	(rundrv),a
	call	runname
	ld	hl,runname_
	; fall into writeto
; writeto: HL -> file name; write the pool in order
writeto: ld	ix,rout
	call	out_open
	ld	hl,(count)
	ld	a,h
	or	l
	jr	z,wt2
	ld	b,h
	ld	c,l
	ld	hl,(ptrs)
wt1:	push	bc
	push	hl
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	ex	de,hl
	ld	ix,rout
	call	out_putrec
	pop	hl
	inc	hl
	inc	hl
	pop	bc
	dec	bc
	ld	a,b
	or	c
	jr	nz,wt1
wt2:	ld	ix,rout
	jp	out_close

; runname: (rundrv), HL = run number -> runname_ "d:Rnnn.TMP"
; (div16 uses DE, so the digits go straight to their places)
runname: ld	a,(rundrv)
	ld	(runname_),a
	ld	bc,100
	call	div16		; HL = n / 100, A = n mod 100
	push	af
	ld	a,l
	add	a,'0'
	ld	(runname_+3),a
	pop	af
	ld	l,a
	ld	h,0
	ld	bc,10
	call	div16		; HL = tens, A = ones
	push	af
	ld	a,l
	add	a,'0'
	ld	(runname_+4),a
	pop	af
	add	a,'0'
	ld	(runname_+5),a
	ret

; ---- merge -----------------------------------------------------
mergeruns: ld	a,'B'
	ld	(indrv),a
	ld	a,'C'
	ld	(outdrv),a
mr1:	ld	hl,(nruns)
	ld	de,2
	or	a
	sbc	hl,de
	ret	c		; fewer than 2 runs: done
	; ngroups = (nruns + maxk - 1) / maxk
	ld	hl,(nruns)
	ld	de,maxk-1
	add	hl,de
	ld	bc,maxk
	call	div16
	ld	(ngroups),hl
	ld	de,m_merge
	call	puts
	ld	hl,(nruns)
	call	putdec
	ld	de,m_into
	call	puts
	ld	hl,(ngroups)
	call	putdec
	ld	de,m_crlf
	call	puts
	ld	hl,1
	ld	(group),hl
mr2:	call	mergegroup
	ld	hl,(group)
	inc	hl
	ld	(group),hl
	ld	de,(ngroups)
	or	a
	sbc	hl,de
	jr	c,mr2
	jr	z,mr2
	ld	hl,(ngroups)
	ld	(nruns),hl
	ld	a,(indrv)
	ld	b,a
	ld	a,(outdrv)
	ld	(indrv),a
	ld	a,b
	ld	(outdrv),a
	jr	mr1

; mergegroup: runs (group-1)*maxk+1 .. min(group*maxk, nruns) from
; indrv into outdrv:Rgroup, or SORTOUT when it is the only group.
mergegroup: ld	hl,(group)
	dec	hl
	add	hl,hl		; *2
	add	hl,hl		; *4
	add	hl,hl		; *8
	add	hl,hl		; *16 = maxk
	inc	hl
	ld	(first),hl
	; nin = min(maxk, nruns - first + 1)
	ld	de,(nruns)
	ex	de,hl
	or	a
	sbc	hl,de
	inc	hl
	ld	a,h
	or	a
	jr	nz,mg1
	ld	a,l
	cp	maxk
	jr	c,mg2
mg1:	ld	a,maxk
mg2:	ld	(nin),a
	; open the inputs and prime their heads
	xor	a
	ld	(live),a
	ld	(slot),a
mg3:	ld	a,(slot)
	ld	c,a
	ld	a,(nin)
	cp	c
	jr	z,mg5
	call	slotix		; IX -> stream for slot C
	ld	a,(indrv)
	ld	(rundrv),a
	ld	hl,(first)
	ld	e,c
	ld	d,0
	add	hl,de
	call	runname
	ld	hl,runname_
	call	in_open
	push	ix
	pop	de
	ld	hl,i_head
	add	hl,de
	ex	de,hl
	call	in_getline
	jr	c,mg4
	ld	a,(live)
	inc	a
	ld	(live),a
	jr	mg4b
mg4:	ld	a,1
	ld	(ix+i_eof),a
mg4b:	ld	a,(slot)
	inc	a
	ld	(slot),a
	jr	mg3
mg5:	; output
	ld	hl,(ngroups)
	dec	hl
	ld	a,h
	or	l
	jr	nz,mg6
	ld	hl,outname
	jr	mg7
mg6:	ld	a,(outdrv)
	ld	(rundrv),a
	ld	hl,(group)
	call	runname
	ld	hl,runname_
mg7:	ld	ix,rout
	call	out_open
	; the merge loop
mg8:	ld	a,(live)
	or	a
	jr	z,mg12
	ld	a,0FFh
	ld	(best),a
	xor	a
	ld	(slot),a
mg9:	ld	a,(slot)
	ld	c,a
	ld	a,(nin)
	cp	c
	jr	z,mg11
	call	slotix
	ld	a,(ix+i_eof)
	or	a
	jr	nz,mg10
	ld	a,(best)
	cp	0FFh
	jr	z,mg9b		; first live head
	push	ix
	pop	hl
	ld	de,i_head
	add	hl,de
	push	hl
	ld	a,(best)
	ld	c,a
	call	slotix
	push	ix
	pop	hl
	ld	de,i_head
	add	hl,de
	pop	de		; DE -> head[slot], HL -> head[best]
	ex	de,hl
	call	cmprec		; head[slot] vs head[best]
	jr	nc,mg10		; not less: keep best
mg9b:	ld	a,(slot)
	ld	(best),a
mg10:	ld	a,(slot)
	inc	a
	ld	(slot),a
	jr	mg9
mg11:	ld	a,(best)
	ld	c,a
	call	slotix
	push	ix
	pop	hl
	ld	de,i_head
	add	hl,de
	push	hl
	push	ix
	ld	ix,rout
	call	out_putrec
	pop	ix
	pop	de
	call	in_getline
	jr	nc,mg8
	ld	a,1
	ld	(ix+i_eof),a
	ld	a,(live)
	dec	a
	ld	(live),a
	jp	mg8
mg12:	ld	ix,rout
	call	out_close
	; close and erase the inputs (a fresh FCB from the name: the
	; one just read through carries extent state the erase minds)
	xor	a
	ld	(slot),a
mg13:	ld	a,(slot)
	ld	c,a
	ld	a,(nin)
	cp	c
	ret	z
	call	slotix
	call	in_close
	ld	a,(indrv)
	ld	(rundrv),a
	ld	a,(slot)
	ld	e,a
	ld	d,0
	ld	hl,(first)
	add	hl,de
	call	runname
	ld	hl,runname_
	call	fcbde
	call	parsefcb
	call	fcbde
	ld	c,b_erase
	call	bdos
	ld	a,(slot)
	inc	a
	ld	(slot),a
	jr	mg13

; slotix: C = slot -> IX = mins + C*i_size (preserves C)
slotix:	push	bc
	push	de
	ld	hl,mins
	ld	de,i_size
	ld	a,c
	or	a
	jr	z,sx2
sx1:	add	hl,de
	dec	a
	jr	nz,sx1
sx2:	push	hl
	pop	ix
	pop	de
	pop	bc
	ret

; ---- data ------------------------------------------------------
m_in:	db	"SORTIN?",13,10,"$"
m_out:	db	"SORTOUT?",13,10,"$"
m_deck:	db	"SYSIN?",13,10,"$"
m_nokeys: db	"SORT: NO SORT FIELDS IN THE DECK",13,10,"$"
m_noopen: db	"SORT: CANNOT OPEN FILE",13,10,"$"
m_full:	db	"SORT: DISK FULL",13,10,"$"
m_done:	db	"SORT: $"
m_runs:	db	"SORT: $"
m_formed: db	" RUNS FORMED",13,10,"$"
m_merge: db	"SORT: MERGE $"
m_into:	db	" RUNS INTO $"
m_crlf:	db	13,10,"$"
m_recs:	db	" RECORDS, END OF SORT",13,10,"$"

conbuf:	ds	34
dbuf:	ds	6
inname:	ds	32
outname: ds	32
deckname: ds	32
runname_: db	"B:R000.TMP",0
rundrv:	db	'B'
indrv:	db	'B'
outdrv:	db	'C'
nkeys:	db	0
keys:	ds	maxkeys*k_size
cra:	dw	0
crb:	dw	0
bytea:	db	0
byteb:	db	0
ptrs:	dw	0
poolend: dw	0
poolnext: dw	0
newnext: dw	0
newrec:	dw	0
count:	dw	0
total:	dw	0
nruns:	dw	0
ngroups: dw	0
group:	dw	0
first:	dw	0
lo:	dw	0
hi:	dw	0
mid:	dw	0
nin:	db	0
live:	db	0
slot:	db	0
best:	db	0
pending: db	0
line:	ds	256
deckmax	equ	2047
deckbuf: ds	deckmax+1
rout:	ds	o_size
rin:	ds	i_size
mins:	ds	maxk*i_size
	ds	256
stack:
poolbase:
	end
