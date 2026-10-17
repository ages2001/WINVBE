#!/usr/bin/env python3
"""mkld.py <ld> <out.x> : the default PE linker script of <ld> with .bss
merged into .data, so that every section has file data and (with a
section alignment below the page size) RVA == file offset, as NT 3.x/4.0
require."""
import subprocess, sys, re
out = subprocess.run([sys.argv[1], '--verbose'], capture_output=True, text=True).stdout
s = out.split('==================================================')[1]
bss = re.search(r'\n  \.bss BLOCK\(__section_alignment__\) :\n  \{.*?\n  \}\n', s, re.S)
s = s.replace(bss.group(0), '\n')
s = s.replace('    __data_end__ = . ;\n', '    *(.bss)\n    *(.bss.*)\n    *(COMMON)\n    __data_end__ = . ;\n', 1)
open(sys.argv[2], 'w').write(s)
