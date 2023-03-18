import time
import sys
import re

if len(sys.argv) != 7:
    exit(-1)

VERSION = sys.argv[1]
PATCHLEVEL = sys.argv[2]
SUBLEVEL = sys.argv[3]
ARCH = sys.argv[4]

input_file = open(sys.argv[5], 'r')
output_file = open(sys.argv[6], 'w')

versionCode = (int(VERSION) << 16) | (int(PATCHLEVEL) << 8) | int(SUBLEVEL)
output_file.write("CONFIG_VERSION_CODE = %u\n" % (versionCode))
output_file.write("CONFIG_ARCH = \"%s\"\n" % (ARCH))

buildInfo = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime())
output_file.write("CONFIG_BUILD_INFO = \"%s\"\n" % (buildInfo))

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
    if data == '':
        output_file.write('# ' + config + ' is not set\n')
        continue

    output_file.write(config + ' = ' + data + '\n')

input_file.close()
output_file.close()
