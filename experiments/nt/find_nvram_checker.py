import subprocess, time, struct

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(12)

with open(f'/proc/{p.pid}/maps') as f:
    maps = f.readlines()

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
                    pos = chunk.find(b'The problem areas are:')
                    if pos != -1:
                        print(f'Found "The problem areas are:" at offset {hex(pos)}')
                        # Let's search instructions in chunk that reference pos
                        # pos is around 0x9000-0xa000
                        # Let's search for lda instructions with displacement pos & 0xffff
                        target_disp = pos & 0xffff
                        for i in range(0, pos, 4):
                            iw = struct.unpack('<I', chunk[i:i+4])[0]
                            disp = iw & 0xffff
                            if disp == target_disp:
                                print(f'Instruction at {hex(i)}: {hex(iw)} references disp {hex(disp)}')
                                # Disassemble 30 instructions around i
                                for j in range(max(0, i-40), min(pos, i+200), 4):
                                    w = struct.unpack('<I', chunk[j:j+4])[0]
                                    print(f'  {hex(0x20000000 + j)}: {hex(w)}')
                        break
                except:
                    pass

p.terminate()
