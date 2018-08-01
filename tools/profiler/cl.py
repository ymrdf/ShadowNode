#!/usr/bin/env python

"""
convert jerry cpu profiler file into collapse file,
which is input format of flamegraph.pl
"""

import sys
import re

# input format
# eachline is a call stack info
# time, bytecode_1,..., bytecode_n-1, bytecode_n
# bytecode_n call bytecode_n-1, ... call bytecode_1
# time is top frame bytecode_n's time

# Merge all same call stack and put into hashmap
def merge_stack(lines, stack_time):
    for line in lines:
        current_line = line.strip('\n')
        stack_line = current_line.split(',', 1)
        if stack_time.has_key(stack_line[1]):
            time = stack_time[stack_line[1]]
        else:
            time = 0.0
        time += float(stack_line[0])
        stack_time[stack_line[1]] = time

def record_stack(lines, stack_time):
    for line in lines:
        current_line = line.strip('\n')
        stack_line = current_line.split(',', 1)
        time = float(stack_line[0])
        stack_time[stack_line[1]] = time

# For each call stack, subtract all call stack whose depth is just 1 smaller.
# The resut is self time.
def compute_self(stack_time):
    for line, time in stack_time.items():
        for line1, time1 in stack_time.items():
            if line1.endswith(line):
                words = line.split(',')
                words1 = line1.split(',')
                if len(words) + 1 == len(words1):
                    time -= time1
        stack_time[line] = time

# parse debug info file into debug_info
def parse_debug_info(path, debug_info):
    for line in path:
        line = line.strip('\n')
        if line.endswith(':'):
            file_name = line.strip(':')
        else:
            m = re.match \
                (r'(\+ ([a-zA-Z0-9_]*))?( )*(\[(\d+),(\d+)\])?( )*(\d+)', line)
            # m.group(1) (\+ ([a-zA-Z0-9_]*))
            # m.group(2) ([a-zA-Z0-9_]*)
            # m.group(3) ( )
            # m.group(4) (\[(\d+),(\d+)\])
            # m.group(5) (\d+)
            # m.group(6) (\d+)
            # m.group(7) ( )
            # m.group(8) (\d+)
            if m:
                func_name = m.group(2) if m.group(2) else m.group(8)
                lineinfo = '(' + m.group(5) + ':' + m.group(6) + ')' \
                           if m.group(4) else '()'
                debug_info[m.group(8)] = func_name + '@' + file_name + lineinfo

# map call stack into human readable
def stacktime_to_human(stack_time, debug_info):
    for line, time in stack_time.items():
        stack = line.split(',')
        stack.reverse()
        display_stack = [debug_info[x] if debug_info.has_key(x) else x \
                        for x in stack]
        print ";".join(display_stack) + ' ' + str(stack_time[line])

def parse_tracing(lines, stack_time, debug_info_file, debug_info):
    merge_stack(lines, stack_time)
    compute_self(stack_time)
    parse_debug_info(debug_info_file, debug_info)
    stacktime_to_human(stack_time, debug_info)

def parse_sampling(lines, stack_time, debug_info_file, debug_info):
    record_stack(lines, stack_time)
    parse_debug_info(DEBUG_INFO_FILE, DEBUG_INFO)
    stacktime_to_human(STACK_TIME, DEBUG_INFO)

PERF_FILE = open(sys.argv[1])
LINE = PERF_FILE.readline()
LINES = PERF_FILE.readlines()
DEBUG_INFO_FILE = open(sys.argv[2])
DEBUG_INFO = {}
STACK_TIME = {}
TYPE = sys.argv[3]

if TYPE == "js":
    parse_tracing(LINES, STACK_TIME,DEBUG_INFO_FILE,DEBUG_INFO)
elif TYPE == "builtin" or TYPE == "gc" or TYPE == "alloc":
    parse_sampling(LINES, STACK_TIME,DEBUG_INFO_FILE,DEBUG_INFO)

