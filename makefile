build:
	gcc -c -ggdb src/fs.c -o obj/fs.o
	ar rcs bin/libfs.a obj/fs.o

setup:
	mkdir obj bin
