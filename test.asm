SALUDO		.EQU	543 + $5
JORL23		.EQU	SALUDO + 3
		.ORG	$1000
		enter	.FRAME
		bra	.ESPERA
		li	r0, .PAPI
		li	r1, 1
		add	r2, r14, -5
.ESPERA 	nop
		eret
		sret
		ret
.FRAME		.EQU	-2
.PAPI		.BYTE	 0, "HOLA", 13
