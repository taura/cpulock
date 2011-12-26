CC:=gcc
CFLAGS:=-Wall -g -lpthread
all : cpulock

cpulock : cpulock.o usage.h 
	$(CC) $(CFLAGS) -o cpulock cpulock.c

usage.h : usage.txt
	sed 's/"/\\"/g' usage.txt > a.h
	awk  '{ print "\"" $$0 "\\n\"" }' a.h > b.h
	mv b.h usage.h
	rm a.h
