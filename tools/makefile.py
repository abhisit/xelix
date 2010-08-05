#!/usr/bin/python
import os;
import re;

# current working directory must be the main decore directory with the Makefile inside!

makefile = open("Makefile", "w");

cfiles = [];
hfiles = [];
asmfiles = []

for root, dirs, files in os.walk("."):
	if ".git" in root:
		continue
	for f in files:
		dateiname = (root + "/" + f)[2:];
		if dateiname[-2:] == ".c":
			cfiles.append(dateiname);
		if dateiname[-2:] == ".h":
			hfiles.append(dateiname);
		if dateiname[-4:] == ".asm":
			asmfiles.append(dateiname);

cfiles.sort();
hfiles.sort();
asmfiles.sort();


makefile.write("# kernel binary\n");
makefile.write("kernel.bin: ");
for f in asmfiles:
	makefile.write(" " + f[:-4] + "-asm.o");
for f in cfiles:
	makefile.write(" " + f[:-2] + ".o");
makefile.write("\n\tld -T linker.ld -nostdlib -o kernel.bin $^\n\n");

makefile.write("# dependencies\n");
for f in hfiles + cfiles:
	makefile.write(f + ":");
	fread = open(f, "r");
	for line in fread:
		m = re.search("#include <(.+)>", line);
		if m != None:
			makefile.write(" " + m.group(1));
	makefile.write("\n");

makefile.write("\n# clean\n");
makefile.write("clean:\n\trm -rf kernel.bin");
for f in asmfiles:
	makefile.write(" " + f[:-4] + "-asm.o");
for f in cfiles:
	makefile.write(" " + f[:-2] + ".o");

makefile.write("""\n\n

# how to compile .c to .o
%.o: %.c
	gcc -Wall -I . -ffreestanding -fno-stack-protector -o $@ -c $<

# how to compile file.asm to file-asm.o (rather than file.o because there exist c files with the same name, i.e. idt.c and and idt.asm would both correspond to idt.o)
%-asm.o: %.asm
	nasm -f elf -o $@ $<


run:
	qemu -kernel kernel.bin

test: kernel.bin run

makefile:
	tools/makefile.py
""");




makefile.close();

