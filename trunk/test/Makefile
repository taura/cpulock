# 
# 
# 

CFLAGS:=-g -Wall
LDFLAGS:=-lpthread
CC:=gcc
n:=20
n_cores:=$(shell cat /proc/cpuinfo  | grep processor | wc -l)
tasks:=$(shell seq 1 $(n))


all : core_watcher
	@echo "NOTE: 'make -j test > FILE' to run a test "

core_watcher : core_watcher.o
	$(CC) -o core_watcher $(LDFLAGS) $(CFLAGS) core_watcher.o

test : $(tasks) 

selected:=3
required:=3
seconds:=5

# 
$(tasks) : % : core_watcher 
	@../src/cpulock -c $(shell ./random_select.py $(selected) $(n_cores)) -n $(required) ./core_watcher $* $(required) $(seconds)

