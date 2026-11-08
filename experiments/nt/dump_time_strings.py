import subprocess, time, sys

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(12)

with open(f'/proc/{p.pid}/maps') as f:
    maps = f.readlines()

found = None
with open(f'/proc/{p.pid}/mem', 'rb', 0) as mem:
    for line in maps:
        parts = line.split()
        if 'rw' in parts[1] and not parts[-1].endswith('.so') and '[' not in parts[-1]:
            start, end = [int(x, 16) for x in parts[0].split('-')]
            size = end - start
            if size > 65536:
                try:
                    mem.seek(start)
                    chunk = mem.read(size)
                    pos = chunk.find(b'Use the arrow keys to adjust the time')
                    if pos != -1:
                        print(f'Found time string at offset {hex(pos)} (host {hex(start + pos)})')
                        print('Surrounding strings:')
                        import re
                        for s in re.findall(b'[\x20-\x7e]{4,}', chunk[max(0, pos-500) : pos+500]):
                            print('  ', s.decode('latin1'))
                        found = (start, pos, chunk)
                        break
                except:
                    pass

p.terminate()
