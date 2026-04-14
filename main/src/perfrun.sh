#!/bin/bash

# perf stat -e cycles,instructions,cache-misses,cache-references,l1d-loads,l1d-load-misses,branches,branch-misses  ./main
# perf record -e cpu-cycles,instructions -F 99 -g --call-graph=dwarf ./main -f ../main.foo
perf record -a -k mono -g -T --sample-cpu --all-user -F 10000 --call-graph=dwarf ./main -f ../main4.foo
