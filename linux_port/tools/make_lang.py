"""Build the game's extra language files from lang/src/<CODE>.txt.

    make_lang.py <lang_ENG.txt from the .app> <lang dir>

The F10 menu: lang/src/menu_ENG.txt lists its English strings, menu_<CODE>.txt their
translations line for line; output <lang dir>/menu_<CODE>.txt ("English<TAB>translation",
accents kept: the menu uses TrueType fonts).

Each source holds the 104 interface lines (0-103 of lang_ENG.txt) followed by the 10
country names (lines 169-178); every other line (driver names, music credits, car names)
is kept from the English file.  Output: <lang dir>/lang_<CODE>.txt, CRLF like the original.

* Italian, French, Spanish, Portuguese: the game's bitmap font only has A-Z, digits and a
  few symbols, so accents are dropped (lower case is drawn as small capitals anyway).
* Arabic: shaped into presentation forms and stored in visual (left-to-right) order, so
  the runtime can draw it glyph by glyph without a shaping engine.
* Russian, Arabic, Chinese, Japanese are drawn by runtime/text.c with the fonts subset
  here (Noto, SIL Open Font License) to the characters actually used.
"""
import os, sys, unicodedata

LATIN = ['ITA', 'FRA', 'SPA', 'POR']
TTF = ['RUS', 'ARA', 'CHI', 'JAP']
# characters the bitmap font has ('#$%&*@' are icons, '[\]^_' blank); tab and '#' are in
# the game's own lines
FONT_OK = set("\t# !\"'()+,-./0123456789:;<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")

NOTO = '/usr/share/fonts/noto/'
CJK = '/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc'
FONTS = {  # output name: (source, ttc index)
    'LAT': (NOTO + 'NotoSans-Bold.ttf', None),
    'ARA': (NOTO + 'NotoSansArabic-Bold.ttf', None),
    'CHI': (CJK, 2),   # Noto Sans CJK SC
    'JAP': (CJK, 0),   # Noto Sans CJK JP
}


def ascii_fold(s):
    s = s.replace('¡', '').replace('¿', '')
    s = ''.join(c for c in unicodedata.normalize('NFD', s) if not unicodedata.combining(c))
    bad = set(s) - FONT_OK
    if bad: raise SystemExit('characters missing from the game font in %r: %r' % (s, bad))
    return s


# ---------------------------------------------------------------- Arabic shaping
# letter: (isolated, final, initial, medial); two forms = joins only to the previous letter
AR = {
    0x621: (0xFE80,), 0x622: (0xFE81, 0xFE82), 0x623: (0xFE83, 0xFE84), 0x624: (0xFE85, 0xFE86),
    0x625: (0xFE87, 0xFE88), 0x626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C), 0x627: (0xFE8D, 0xFE8E),
    0x628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92), 0x629: (0xFE93, 0xFE94),
    0x62A: (0xFE95, 0xFE96, 0xFE97, 0xFE98), 0x62B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C),
    0x62C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0), 0x62D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4),
    0x62E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8), 0x62F: (0xFEA9, 0xFEAA), 0x630: (0xFEAB, 0xFEAC),
    0x631: (0xFEAD, 0xFEAE), 0x632: (0xFEAF, 0xFEB0), 0x633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),
    0x634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8), 0x635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),
    0x636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0), 0x637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),
    0x638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8), 0x639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),
    0x63A: (0xFECD, 0xFECE, 0xFECF, 0xFED0), 0x641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),
    0x642: (0xFED5, 0xFED6, 0xFED7, 0xFED8), 0x643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),
    0x644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0), 0x645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),
    0x646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8), 0x647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),
    0x648: (0xFEED, 0xFEEE), 0x649: (0xFEEF, 0xFEF0), 0x64A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),
}
LAM_ALEF = {0x622: (0xFEF5, 0xFEF6), 0x623: (0xFEF7, 0xFEF8), 0x625: (0xFEF9, 0xFEFA), 0x627: (0xFEFB, 0xFEFC)}
MIRROR = {'(': ')', ')': '(', '<': '>', '>': '<', '[': ']', ']': '['}


def shape_arabic(s):
    cps = [ord(c) for c in s]
    out = []
    i = 0
    while i < len(cps):
        c = cps[i]
        if c not in AR: out.append(c); i += 1; continue
        prev_joins = i > 0 and cps[i - 1] in AR and len(AR[cps[i - 1]]) == 4
        if c == 0x644 and i + 1 < len(cps) and cps[i + 1] in LAM_ALEF:
            out.append(LAM_ALEF[cps[i + 1]][1 if prev_joins else 0]); i += 2; continue
        nxt = cps[i + 1] if i + 1 < len(cps) else None
        next_joins = len(AR[c]) == 4 and nxt in AR
        f = AR[c]
        if len(f) == 1: out.append(f[0])
        elif prev_joins and next_joins: out.append(f[3])
        elif prev_joins: out.append(f[1])
        elif next_joins: out.append(f[2])
        else: out.append(f[0])
        i += 1
    return ''.join(map(chr, out))


def visual_order(s):
    """Right-to-left line: reverse it, keeping runs of digits / Latin letters left-to-right."""
    if not any(0x600 <= ord(c) <= 0x6FF or 0xFE70 <= ord(c) <= 0xFEFF for c in s): return s
    runs, cur, cur_ltr = [], '', None
    for i, c in enumerate(s):
        ltr = c.isascii() and c.isalnum()
        if c in '.:' and cur_ltr and s[i + 1:i + 2].isdigit(): ltr = True   # keep "1.5", "3:2" together
        if cur and ltr != cur_ltr: runs.append((cur, cur_ltr)); cur = ''
        cur += c; cur_ltr = ltr
    if cur: runs.append((cur, cur_ltr))
    return ''.join(r if ltr else ''.join(MIRROR.get(c, c) for c in reversed(r)) for r, ltr in reversed(runs))


NAMES = [('ITA', 'Italiano'), ('ENG', 'English'), ('FRA', 'Français'), ('ARA', 'العربية'), ('SPA', 'Español'),
         ('POR', 'Português (Brasil)'), ('RUS', 'Русский'), ('CHI', '中文'), ('JAP', '日本語')]


def read_lines(path):
    lines = open(path, encoding='utf-8').read().split('\n')
    if lines and lines[-1] == '': lines.pop()
    return [l.rstrip('\r') for l in lines]


def build(eng_path, lang_dir):
    eng = open(eng_path, 'rb').read().decode('latin-1').split('\r\n')
    used = {k: set() for k in FONTS}
    for code in LATIN + TTF:
        src = open(os.path.join(lang_dir, 'src', code + '.txt'), encoding='utf-8').read().split('\n')
        src = [l.rstrip('\r') for l in src]
        if src and src[-1] == '': src.pop()
        if len(src) != 114: raise SystemExit('%s: %d lines, expected 114' % (code, len(src)))
        lines = list(eng)
        for i in range(104): lines[i] = src[i]
        for i in range(10): lines[169 + i] = src[104 + i]
        if code in LATIN: lines = [ascii_fold(l) for l in lines]
        if code == 'ARA': lines = [visual_order(shape_arabic(l)) for l in lines]
        text = ''.join(lines)
        if code == 'ARA': used['ARA'] |= set(text)
        if code in ('CHI', 'JAP'): used[code] |= set(text)
        if code in TTF: used['LAT'] |= set(text) | {c.upper() for c in text}
        with open(os.path.join(lang_dir, 'lang_%s.txt' % code), 'wb') as f:
            f.write('\r\n'.join(lines).encode('utf-8'))
    # F10 menu
    menu_en = read_lines(os.path.join(lang_dir, 'src', 'menu_ENG.txt'))
    for code in LATIN + TTF:
        tr = read_lines(os.path.join(lang_dir, 'src', 'menu_%s.txt' % code))
        if len(tr) != len(menu_en): raise SystemExit('menu_%s: %d lines, expected %d' % (code, len(tr), len(menu_en)))
        if code == 'ARA': tr = [visual_order(shape_arabic(l)) for l in tr]
        text = ''.join(tr)
        if code in ('ARA', 'CHI', 'JAP'): used[code] |= set(text)
        if code == 'RUS': used['LAT'] |= set(text)
        with open(os.path.join(lang_dir, 'menu_%s.txt' % code), 'wb') as f:
            f.write(''.join('%s\t%s\n' % (e, t) for e, t in zip(menu_en, tr)).encode('utf-8'))
    # language names, shown in their own script whatever the menu language
    with open(os.path.join(lang_dir, 'names.txt'), 'wb') as f:
        for code, name in NAMES:
            if code == 'ARA': name = visual_order(shape_arabic(name))
            f.write(('%s\t%s\n' % (code, name)).encode('utf-8'))
            for k in ('ARA', 'CHI', 'JAP', 'LAT'): used[k] |= set(name)
    ascii_chars = {chr(c) for c in range(0x20, 0x7f)}
    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        print('fontTools missing: fonts not regenerated'); return
    for name, (path, index) in FONTS.items():
        chars = used[name] | ascii_chars
        font = TTFont(path, fontNumber=index if index is not None else -1, lazy=False)
        opts = subset.Options()
        opts.layout_features = []      # glyphs drawn one by one: no GSUB/GPOS needed
        opts.name_IDs = ['*']
        opts.notdef_outline = True
        opts.hinting = False
        s = subset.Subsetter(opts)
        s.populate(unicodes=[ord(c) for c in chars])
        s.subset(font)
        font.save(os.path.join(lang_dir, 'font_%s.ttf' % name))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2])
