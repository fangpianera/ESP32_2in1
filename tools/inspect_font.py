import re, collections, sys

for p in sys.argv[1:]:
    s = open(p, encoding='utf-8', errors='ignore').read()
    i = s.index("glyph_dsc[] = {")
    j = s.index("};", i)
    blk = s[i:j]
    ents = re.findall(r"\.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)", blk)
    adv = collections.Counter(int(e[0]) for e in ents)
    cjk = [e for e in ents if int(e[1]) >= 16]
    print("==", p.split("\\")[-1], "glyphs", len(ents), "cjk-like", len(cjk))
    print("   top adv_w:", adv.most_common(5))
    print("   cjk sample:", cjk[:6])
    for k in ("line_height", "base_line"):
        m = re.search(r"\.%s = (-?\d+)" % k, s)
        print("  ", k, m.group(1) if m else "NA")
