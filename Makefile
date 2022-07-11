eonasm: eonasm.c
	musl-gcc -O2 -Wall -static -std=c11 -s -o $@ $^
test:
	rm -f /tmp/eon.ihex && ./eonasm -u -l /tmp/eon.ihex test.asm
