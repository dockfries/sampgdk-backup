#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Convert open.mp Pawn native declarations (from .inc files) into sampgdk IDL.

Follows the conventions of the existing sampgdk IDL files:
  - Tagged types are stripped: bool: -> bool, Float: -> float, other tags -> int
  - Output references (&x) become [out] <type> x
  - const x[] input arrays become string x; &x[] output arrays become [out] string x
  - Default values are preserved when they are plain constants, dropped otherwise
  - Natives that cannot be represented (varargs {...:...}, #property accessors,
    true multi-dimensional arrays) are skipped with a warning
"""
import re
import sys

TAG_MAP = {
    'bool': 'bool',
    'Float': 'float',
    'string': 'string',
}
# any other tag maps to int

# Parameter names that collide with C types/keywords used by the generated
# code (sampgdk's cell, and the `string` type name). They get a trailing
# underscore so the generated C compiles.
RESERVED_PARAM_NAMES = {'cell', 'string'}

def split_params(s):
    """Split a Pawn parameter list on top-level commas (ignore commas inside
    brackets/parens/braces)."""
    params = []
    depth = 0
    cur = []
    for ch in s:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            params.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        params.append(''.join(cur).strip())
    return params

def map_type(c_type):
    """Map a Pawn type (possibly tagged / array / reference) to an IDL
    (type, is_out, is_skippable) tuple."""
    c_type = c_type.strip()

    if c_type == '#':
        return None, False, True  # #World() accessor

    # varargs marker like {Float, _}:... handled by caller

    is_out = False
    if c_type.startswith('&'):
        is_out = True
        c_type = c_type[1:].strip()

    # const prefix must be stripped before the array check, since
    # `const x[]` arrives here as `const[]` (type 'const' + array suffix).
    is_const = c_type.startswith('const')
    if is_const:
        c_type = c_type[len('const'):].strip()

    # array suffix []
    is_array = c_type.endswith('[]')
    if is_array:
        c_type = c_type[:-2].strip()

    # strip tag (TYPE:name)
    tag = None
    if ':' in c_type:
        tag, c_type = c_type.split(':', 1)
        tag = tag.strip()

    if c_type == '_':
        # unnamed/vararg placeholder, skip
        return None, is_out, True

    if c_type == '...':
        return None, is_out, True  # varargs

    base = c_type.strip()

    if is_array:
        # const x[] -> input string; non-const x[] -> output buffer ([out] string)
        return 'string', is_out or not is_const, False

    # recognize the three IDL types by tag OR bare name (e.g. `bool x`,
    # `Float x`, `bool:IsValidGangZone`)
    if base in ('bool',) or tag in ('bool',):
        return 'bool', is_out, False
    if base in ('Float', 'float') or tag in ('Float', 'float'):
        return 'float', is_out, False
    if base in ('string',) or tag in ('string',):
        return 'string', is_out, False

    # unknown tag or no tag -> int
    return 'int', is_out, False

def is_varargs(params_str):
    return '{' in params_str and '...' in params_str

def convert(src, dst):
    with open(src, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()

    # Strip /* */ comment blocks: they contain `native # Section();`
    # doxygen group markers whose continuation lines (`native Section(`)
    # would otherwise be misparsed as real natives.
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)

    lines = [l.strip() for l in text.splitlines() if l.strip()]
    out = []
    skipped = []
    seen = set()

    for line in lines:
        # only process native declarations
        if not line.startswith('native '):
            continue

        decl = line[len('native '):].strip()
        # drop trailing ; if present
        if decl.endswith(';'):
            decl = decl[:-1].strip()

        # alias: `native X(...) = Y;` — X is a compile-time alias for the
        # real native Y. Only Y is registered by the server, so X must be
        # skipped entirely (a wrapper that calls X would fail at runtime).
        eq = decl.find('=')
        if eq >= 0:
            # find '=' that is outside parens
            depth = 0
            real_eq = -1
            for i, ch in enumerate(decl):
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
                elif ch == '=' and depth == 0:
                    real_eq = i
                    break
            if real_eq >= 0:
                skipped.append((decl[:60], 'alias'))
                continue

        # #property accessor: native #World()
        if decl.startswith('#'):
            skipped.append((decl, 'property accessor'))
            continue

        # varargs: native printf(const format[], {Float, _}:...);
        # Keep the fixed parameters, drop the varargs tail (sampgdk IDL
        # cannot represent `{...}:...` or `TAG:...`, but the fixed part is
        # still usable).
        if '{' in decl or ':...' in decl:
            decl = re.sub(r',\s*(\{[^}]*\}|[A-Za-z_][A-Za-z_0-9]*)\s*:\s*\.\.\.', '', decl)
            # if nothing but varargs remains, skip
            lp = decl.find('(')
            inner = decl[lp+1:].strip() if lp >= 0 else ''
            if not inner or set(inner.replace(')', '').strip()) <= {' '}:
                skipped.append((decl[:60], 'varargs only'))
                continue

        # split return type and function name: return is `Type` or `Tag:Name`
        lp = decl.find('(')
        if lp < 0:
            skipped.append((decl[:60], 'no paren'))
            continue
        head, params_str = decl[:lp].strip(), decl[lp+1:].strip()
        if params_str.endswith(')'):
            params_str = params_str[:-1].strip()

        # head is like `bool:SetPlayerWantedLevel` or `GetPlayerWantedLevel`
        if ':' in head:
            ret_tagged, name = head.rsplit(':', 1)
        else:
            # return type and name separated by space (e.g. `int GetX()`)
            sp = head.rfind(' ')
            if sp < 0:
                ret_tagged, name = 'int', head
            else:
                ret_tagged, name = head[:sp].strip(), head[sp:].strip()

        # return type may be tagged: bool:SetPlayerWantedLevel(...)
        ret_type, ret_is_out, ret_skip = map_type(ret_tagged)
        if ret_skip or ret_is_out:
            skipped.append((decl[:60], 'ret type'))
            continue

        if not re.match(r'^[A-Za-z_][A-Za-z_0-9]*$', name):
            skipped.append((decl[:60], 'bad name'))
            continue

        if name in seen:
            continue
        seen.add(name)

        # split params
        params = []
        ok = True
        for p in split_params(params_str):
            p = p.strip()
            if not p:
                continue
            # param: [type] name[]? [= default]
            pm = re.match(r'^(.*?)\s*([A-Za-z_][A-Za-z_0-9]*)(\[\])?\s*(=\s*(.*))?$', p, re.DOTALL)
            if not pm:
                skipped.append((decl[:60], 'bad param %r' % p))
                ok = False
                break
            ptype_tagged, pname, p_array, _, pdefault = pm.group(1).strip(), pm.group(2), pm.group(3), pm.group(4), pm.group(5)
            if p_array:
                ptype_tagged = ptype_tagged + '[]' if ptype_tagged else '[]'
            if pname in RESERVED_PARAM_NAMES:
                pname = pname + '_'
            ptype, p_is_out, p_skip = map_type(ptype_tagged)
            if p_skip:
                skipped.append((decl[:60], 'skip param %s' % p))
                ok = False
                break
            if p_is_out:
                params.append('[out] %s %s' % (ptype, pname))
            else:
                # keep only literal default values (numbers, true/false);
                # drop references to enum constants and function calls
                # (sizeof(...), WEAPON_FIST, etc.) which the IDL cannot resolve.
                if pdefault is not None:
                    pd = pdefault.strip()
                    lit = re.match(r'^[+-]?(0[xX][0-9a-fA-F]+|[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?)$', pd)
                    if lit or pd in ('true', 'false'):
                        params.append('%s %s = %s' % (ptype, pname, pd))
                    else:
                        params.append('%s %s' % (ptype, pname))
                else:
                    params.append('%s %s' % (ptype, pname))

        if not ok:
            continue

        # C++ requires default arguments to be trailing and contiguous. The
        # extraction may have dropped some (e.g. WEAPON_FIST) while keeping
        # numeric ones (ammo1 = 0), producing a non-contiguous list that
        # would not compile. If defaults are non-contiguous, drop them all.
        saw_default = False
        defaults_contiguous = True
        for p in params:
            if '=' in p:
                saw_default = True
            elif saw_default:
                defaults_contiguous = False
                break
        if saw_default and not defaults_contiguous:
            params = [re.sub(r'\s*=\s*[^=]*$', '', p).strip() for p in params]

        out.append('[native] %s %s(%s);' % (ret_type, name, ', '.join(params)))

    with open(dst, 'w', encoding='utf-8') as f:
        f.write('/* Generated from open.mp includes by extract_omp_natives.py */\n')
        f.write('\n'.join(out))
        f.write('\n')

    print('wrote %d natives to %s (%d skipped)' % (len(out), dst, len(skipped)))
    for s in skipped[:30]:
        print('  SKIP: %s... [%s]' % (s[0], s[1]))

if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2])
