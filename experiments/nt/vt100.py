import re

class VT100:
    def __init__(self, rows=25, cols=80):
        self.rows = rows
        self.cols = cols
        self.grid = [[' ' for _ in range(cols)] for _ in range(rows)]
        self.r = 0
        self.c = 0

    def feed(self, data):
        i = 0
        n = len(data)
        while i < n:
            ch = data[i]
            if ch == '\r':
                self.c = 0
                i += 1
            elif ch == '\n':
                self.r = min(self.rows - 1, self.r + 1)
                i += 1
            elif ch == '\b':
                self.c = max(0, self.c - 1)
                i += 1
            elif ch == '\x07': # BEL
                i += 1
            elif ch == '\x1b' or ch == '\x9b':
                # Escape sequence
                is_8bit = (ch == '\x9b')
                seq = ''
                i += 1
                if not is_8bit:
                    if i < n and data[i] == '[':
                        i += 1
                    else:
                        # Other ESC sequence
                        if i < n:
                            i += 1
                        continue
                # Now we have CSI parameters and command
                while i < n and not ('a' <= data[i] <= 'z' or 'A' <= data[i] <= 'Z'):
                    seq += data[i]
                    i += 1
                cmd = data[i] if i < n else ''
                if i < n:
                    i += 1
                self.handle_csi(seq, cmd)
            elif ord(ch) >= 32 and ord(ch) < 127 or ord(ch) >= 160:
                if 0 <= self.r < self.rows and 0 <= self.c < self.cols:
                    self.grid[self.r][self.c] = ch
                    self.c += 1
                i += 1
            else:
                i += 1

    def handle_csi(self, seq, cmd):
        params = [int(p) if p.isdigit() else 0 for p in seq.split(';')] if seq else [0]
        if cmd == 'H' or cmd == 'f': # Cursor position
            r = params[0] - 1 if len(params) > 0 and params[0] > 0 else 0
            c = params[1] - 1 if len(params) > 1 and params[1] > 0 else 0
            self.r = max(0, min(self.rows - 1, r))
            self.c = max(0, min(self.cols - 1, c))
        elif cmd == 'J': # Erase in display
            mode = params[0] if params else 0
            if mode == 2:
                self.grid = [[' ' for _ in range(self.cols)] for _ in range(self.rows)]
                self.r = 0
                self.c = 0
        elif cmd == 'K': # Erase in line
            mode = params[0] if params else 0
            if mode == 0:
                for c in range(self.c, self.cols):
                    self.grid[self.r][c] = ' '
            elif mode == 2:
                for c in range(self.cols):
                    self.grid[self.r][c] = ' '
        elif cmd == 'A': # Cursor up
            count = params[0] if params and params[0] > 0 else 1
            self.r = max(0, self.r - count)
        elif cmd == 'B': # Cursor down
            count = params[0] if params and params[0] > 0 else 1
            self.r = min(self.rows - 1, self.r + count)
        elif cmd == 'C': # Cursor forward
            count = params[0] if params and params[0] > 0 else 1
            self.c = min(self.cols - 1, self.c + count)
        elif cmd == 'D': # Cursor back
            count = params[0] if params and params[0] > 0 else 1
            self.c = max(0, self.c - count)

    def render(self):
        lines = []
        for row in self.grid:
            lines.append("".join(row).rstrip())
        while lines and not lines[-1]:
            lines.pop()
        return "\n".join(lines)
