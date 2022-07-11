# eonasm
eonasm is a classical multipass assembler for [EON](https://github.com/elgron-eon/eon-cpu) cpu.
It can generate listing file and support local labels (prefixed with '.'). Doesn' need a linker (
aka classical assembler which produces binary image directly).

# usage
```
eonasm 0.0.0, classical assembler for eon cpu
usage  :  eonasm [option]* outfile infile+
options:
	-l	listing
	-u	show unused labels
	-v	verbose assembly
```

# build
this repo has a makefile, simply launch **make**

# example listing
  >####################### test.asm
  0000 = 0000.0224	1 SALUDO	  .EQU	  543 + $5
  0000 = 0000.0227	2 JORL23	  .EQU	  SALUDO + 3
  0000			3		  .ORG	  $1000
  1000 0FF8FFFE 	4		  enter   .FRAME
  1004 2FF00005 	5		  bra	  .ESPERA
  1008 30F9101A 	6		  li	  r0, .PAPI
  100C 01F8		7		  li	  r1, 1
  100E 32E4FFFB 	8		  add	  r2, r14, -5
  1012 0FF1		9 .ESPERA	  nop
  1014 0FF6	       10		  eret
  1016 0FF5	       11		  sret
  1018 0FE0	       12		  ret
  101A = FFFF.FFFE     13 .FRAME	  .EQU	  -2
  101A 00484F4C410D    14 .PAPI 	  .BYTE    0, "HOLA", 13
  >#######################     4 passes. global/local labels (MAX   256):     2 /     3
