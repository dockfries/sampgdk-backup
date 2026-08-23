import os
import re
import glob
import sys

# Include directory is passed as a command-line argument so no private
# absolute paths are baked into this file.
if len(sys.argv) < 2:
    print('Usage: gen_omp_consts.py <open.mp-include-dir>')
    sys.exit(1)
INC = sys.argv[1]

# Everything else is derived relative to this script's location.
HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, '..', 'lib', 'sampgdk')

# constants already in sampgdk
existing = set()
for path in glob.glob(os.path.join(LIB, 'a_*.idl')):
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = re.match(r'const\s+int\s+([A-Za-z_][A-Za-z_0-9]*)', line)
            if m:
                existing.add(m.group(1))

# open.mp defines + enums
defines = {}
for path in glob.glob(os.path.join(INC, '*.inc')):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'#define\s+([A-Za-z_][A-Za-z_0-9]*)\s+(.+)', line)
            if m:
                defines[m.group(1)] = m.group(2).strip()

enums = {}
for path in glob.glob(os.path.join(INC, '*.inc')):
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    for em in re.finditer(r'\benum\s+(?:[A-Za-z_][A-Za-z_0-9]*\s*)?\{([^}]*)\}', text):
        body = em.group(1)
        counter = 0
        for item in body.split(','):
            item = item.strip()
            if not item:
                continue
            m = re.match(r'([A-Za-z_][A-Za-z_0-9]*)\s*(?:=\s*(.+))?$', item)
            if m:
                name, val = m.group(1), m.group(2)
                val = val.strip() if val else str(counter)
                enums[name] = val
                if re.match(r'^[+-]?\d+$', val):
                    counter = int(val) + 1
                else:
                    counter += 1

all_omp = dict(defines)
all_omp.update(enums)
new = {k: v for k, v in all_omp.items() if k not in existing}

def clean_value(raw):
    """Return (int_value_str or None). None means skip (cannot represent)."""
    v = raw.strip()
    v = re.sub(r'//.*$', '', v).strip()
    m = re.match(r'^\(\s*[A-Za-z_][A-Za-z_0-9]*\s*:\s*(.+?)\s*\)$', v)
    if m:
        v = m.group(1).strip()
    else:
        m = re.match(r'^[A-Za-z_][A-Za-z_0-9]*\s*:\s*(.+)$', v)
        if m:
            v = m.group(1).strip()
    while v.startswith('(') and v.endswith(')') and v.count('(') == 1:
        v = v[1:-1].strip()
    if re.match(r'^[+-]?(0[xX][0-9a-fA-F]+|\d+)$', v):
        return v
    return None

out_lines = []
skipped = []
for name in sorted(new):
    val = clean_value(new[name])
    if val is not None:
        out_lines.append('const int %-40s = %s;' % (name, val))
    else:
        skipped.append((name, new[name]))

out_path = os.path.join(LIB, 'omp_consts.idl')
with open(out_path, 'w', encoding='utf-8') as f:
    f.write('/* Generated from open.mp includes (const int constants) */\n')
    f.write('\n'.join(out_lines))
    f.write('\n')

print('wrote %d consts to %s; skipped %d' % (len(out_lines), out_path, len(skipped)))
