	.global _start

_start:
	ld.w	%r0, 0
	ext	0
	ext	6144
	ld.w	%r1, 8
	ld.w	[%r1], %r0
	halt
