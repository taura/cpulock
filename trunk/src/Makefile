CC:=gcc
CFLAGS:=-Wall -O3 -lpthread
all : cpulock

cpulock : cpulock.o
	$(CC) $(CFLAGS) -o cpulock cpulock.c

cpulock.o : usage.h

usage.h : ../docs/usage.txt
	sed 's/"/\\"/g' ../docs/usage.txt > a.h
	awk  '{ print "\"" $$0 "\\n\"" }' a.h > b.h
	mv b.h usage.h
	rm a.h
