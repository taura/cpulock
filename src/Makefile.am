bin_PROGRAMS = cpulock
cpulock_SOURCES = cpulock.c ../docs/usage.txt

cpulock.$(OBJEXT) : usage.h

usage.h : ../docs/usage.txt
	sed 's/"/\\"/g' ../docs/usage.txt | $(AWK)  '{ print "\"" $$0 "\\n\"" }' > _usage.h
	mv _usage.h usage.h
