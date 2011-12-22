#!/usr/bin/python

import random,sys
# select a items out of n
def main():
    a = int(sys.argv[1])
    n = int(sys.argv[2])
    L = range(n)
    random.shuffle(L)
    L = L[:a]
    L.sort()
    print ",".join([ ("%s" % x) for x in L ])

main()

