SALUDO		.EQU	543 + $5
JORL23		.EQU	SALUDO + 3
		.ORG	$1000
		enter	.FRAME
		bra	.ESPERA
		li	r0, .PAPI
		li	r1, 1
		add	r2, r14, -5
		mul	r5, 10
		mul	r4, r5, sp
		imul	r4, r7
		idiv	r5, r8
.ESPERA 	nop
		inv	r3, 1024
		eret
		sret
		ret
.FRAME		.EQU	-2
.PAPI		.BYTE	 0, "HOLA", 13, 22
		.ALIGN	4
		.WORD	45
		.LONG	83

		.END
anything beyond .end is ignored
jorl23
