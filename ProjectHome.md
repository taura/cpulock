This is a simple "mutual exclusion" utility for running many processes
so that each is run exclusively on a set of CPU cores.  Its syntax is
similar to taskset utility on Linux. For example,

> cpulock -c 0,3,7 ./your\_program

will acquire three CPUs 0, 3, and 7, waiting for them to become free
if necessary, and then run command ./your\_program.