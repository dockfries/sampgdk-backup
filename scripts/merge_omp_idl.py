import os
import re
import glob

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, '..', 'lib', 'sampgdk')
EXTRACTED = os.path.join(HERE, '..', 'extracted_natives')

NATIVE_RE = re.compile(r'\[native[^\]]*\]\s+[A-Za-z_][A-Za-z_0-9]*\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(')

# Gather all native names already in sampgdk IDLs (a_*.idl in lib/sampgdk)
existing = set()
for path in glob.glob(os.path.join(LIB, 'a_*.idl')):
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = NATIVE_RE.search(line)
            if m:
                existing.add(m.group(1))

# Pawn core/float/string/file/time builtin natives that are not server
# natives (C++ has equivalents) and must not be wrapped.
PAWN_BUILTINS = {
    'argcount','argindex','argstr','argvalue','clamp','deleteproperty',
    'existproperty','fblockread','fblockwrite','fclose','fcreatedir','fexist',
    'fflush','fgetchar','filecrc','flength','float','floatabs','floatadd',
    'floatcmp','floatcos','floatdiv','floatfract','floatlog','floatmul',
    'floatpower','floatround','floatsin','floatsqroot','floatstr','floatsub',
    'floattan','fopen','fputchar','fread','fremove','frename','fseek','fstat',
    'ftell','funcidx','fwrite','getarg','getdate','getproperty','gettime',
    'heapspace','ispacked','max','min','numargs','print','random','setarg',
    'setproperty','strcat','strcmp','strdel','strfind','strins','strlen',
    'strmid','strpack','strunpack','strval','swapchars','tickcount','tolower',
    'toupper',
    # Pawn math/format builtins re-declared in omp_core.inc (C++ has these)
    'acos','asin','atan','atan2','cos','sin','tan','exp','log','pow','sqrt',
    'fabs','fmod','floor','ceil','round','format','Format','PrintF',
    '__printf','printf','sprintf','print',
    # Uppercase aliases of Pawn math builtins (native Float:ACos(...) = acos)
    'ACos','ASin','ATan','ATan2',
}

out_dir = LIB
total_new = 0
for path in sorted(glob.glob(os.path.join(EXTRACTED, 'omp_*.idl'))):
    module = re.match(r'omp_([a-z_]+)\.idl$', path.replace('\\', '/').split('/')[-1]).group(1)
    natives = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line.startswith('[native]'):
                continue
            m = NATIVE_RE.search(line)
            if m:
                natives.setdefault(m.group(1), line)

    new = {k: v for k, v in natives.items()
           if k not in existing and k not in PAWN_BUILTINS and not k.endswith('f')}

    out_path = '%s/omp_%s.idl' % (out_dir, module)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('/* Generated from open.mp include omp_%s.inc; '
                'natives not present in a_*.idl */\n' % module)
        for name in sorted(new):
            f.write(new[name] + '\n')
    total_new += len(new)
    print('omp_%s.idl: %d natives (%d raw)' % (module, len(new), len(natives)))

print('total new:', total_new)
