all:
	gcc fuse-git.c -o fuse-git -lgit2 -lfuse
