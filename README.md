# eonasm
eonasm is a classical multipass assembler for [EON](https://github.com/elgron-eon/eon-cpu) cpu.
It can generate listing file and support local labels (prefixed with '.'). Doesn' need a linker (
aka classical assembler which produces binary image directly).

# usage
```
eonasm 0.0.0, classical assembler for eon cpu
usage  :  eonasm [option]* outfile infile+
options:
        -l      listing
        -u      show unused labels
        -v      verbose assembly
```

# build
this repo has a makefile, simply launch **make**

# example listing
```
```
