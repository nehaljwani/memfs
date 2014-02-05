all:
	gcc -Wall -g -O0 -D_FILE_OFFSET_BITS=64 memfs.c -o MemoryFS_r -lfuse

clean:
	rm -f memfs.o MemoryFS_r
