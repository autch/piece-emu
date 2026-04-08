// crt0.s — bare-metal startup for piece-emu tests.
//
// Sets SP to top of IRAM (0x002000) then jumps to _start_c (C function).
//
// Loading 0x002000 via ext + ld.w:
//   ext  0x80              ; imm13 = 0x80
//   ld.w %r0, 0            ; combined = sign_extend((0x80<<6)|0, 19) = 0x2000
//   ld.w %sp, %r0          ; SP ← 0x2000

	.text
	.globl	_start
	.type	_start, @function
_start:
	ext	0x80
	ld.w	%r0, 0
	ld.w	%sp, %r0
	call	_start_c
	halt
	.size	_start, . - _start
