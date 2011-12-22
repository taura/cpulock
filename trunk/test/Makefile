
CFLAGS:=-g -Wall
LDFLAGS:=-lpthread
CC:=gcc
n:=20
n_cores:=$(shell cat /proc/cpuinfo  | grep processor | wc -l)
tasks:=$(shell seq 1 $(n))




all : core_watcher
tasks : $(tasks) 

selected:=10
required:=5
seconds:=15

# 
$(tasks) : % : core_watcher 
	@ ../corelock -c $(shell ./random_select.py $(selected) $(n_cores)) -n $(required) ./core_watcher $* $(required) $(seconds)

