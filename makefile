build:
	mkdir -p obj
	mkdir -p bin
	gcc -c -ggdb src/fs.c -o obj/fs.o
	ar rcs bin/libfs.a obj/fs.o

setup:
	mkdir -p obj bin

install:
	sudo cp bin/libfs.a /usr/lib
	sudo cp -r src/include/* /usr/lib/include
	sudo chmod 755 /usr/lib/libfs.a

clean:
	rm -rf obj bin
