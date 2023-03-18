import os
import sys
import re

if len(sys.argv) != 3:
    exit(-1)

input_file = open(sys.argv[1], 'r')
output_file = open(sys.argv[2], 'w')

output_file.write("#ifndef __AUTOCFG_H__\n")
output_file.write("#define __AUTOCFG_H__\n\n")

def is_number(s):
    try:
        float(s)
        return True
    except (TypeError, ValueError):
        pass

    return False

for line in input_file.readlines():
    line = line.strip()
    if line[0] == '#':
        continue
    matchObj = re.search( r'.*( *)?=( *)?.*', line, re.M|re.I)
    if matchObj is None:
        continue
    dataInfo = matchObj[0].split('=', 1)
    config = dataInfo[0].strip()
    data = dataInfo[1].strip()
    if data == '' or data == 'n' or data == 'N':
        continue
    if data == 'y' or data == 'Y':
        data = '1'
    elif data == 'm' or data == 'M':
        data = '2'

    output_file.write("#define " + config + ' ' + data + '\n')


output_file.write("\n")
output_file.write("#endif\n")

input_file.close()
output_file.close()
