import os
import sys
import struct

if len(sys.argv) != 7:
    exit(-1)

img_in = open(sys.argv[1], 'rb')
img_out = open(sys.argv[6], 'ab')

img_in_size = os.path.getsize(sys.argv[1])

img_in_data = img_in.read()

list_dec = [0x30, 0x20, 0x30, 0x20, 0, 0, 0, 0, (img_in_size & 0xff), \
    ((img_in_size >> 8) & 0xff), ((img_in_size >> 16) & 0xff), \
    ((img_in_size >> 24) & 0xff), int(sys.argv[5]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])]

for x in list_dec:
        a = struct.pack('B', x)
        img_out.write(a)

img_out.write(img_in_data)

img_in.close
img_out.close