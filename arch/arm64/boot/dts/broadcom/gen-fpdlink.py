#!/usr/bin/python3

import re
import os

path = os.path.dirname(os.path.realpath(__file__))

main = open(path + '/v3link.dtsi', 'w')

with open(path + '/v3link.dtsi.in', 'r') as f:
    data = f.read()
    main.write(data)

with open(path + '/rpi5-fpdlink-arducam-ub953-imx219.dtsi.in', 'r') as f:
    template = f.read()

for port in range(4):
    data = template
    data = re.sub(r'\${nport}', str(port), data)
    data = re.sub(r'\${i2c-alias}', str(port + 0x44), data)

    main.write(data)
