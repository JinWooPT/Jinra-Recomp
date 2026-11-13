"""The Insert menu's sections: heading rows group a tab's rows, and none can be left empty (gpu/vk/vk_menu.cpp).

A heading row groups settings, and keyboard selection skips it. Image reconstruction and display options belong in
Picture. Frame rate and video memory belong in Performance. A heading still needs a setting when unsupported choices
are hidden. These checks read the rows directly from the source.

    python -I -B tests/test_menu_sections.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GRAPHICS = [
    ("Resolution and upscaling", ["Aspect ratio", "Output resolution", "Profile", "Upscaler", "DLSS model", "Render resolution", "Sharpness"]),
    ("Screen", ["Fullscreen", "V-Sync", "Window scaling filter", "Ultrawide menus"]),
    ("HDR", ["HDR", "HDR paper white", "HDR peak brightness", "HDR contrast", "HDR tone mapper"]),
    ("Detail", ["Anisotropic filtering"]),
]
PERFORMANCE = [
    ("Frame generation and latency", ["Game frame rate cap", "Frame generation", "Frame generation by", "Frame pacing",
                                      "NVIDIA Reflex", "AMD Anti-Lag 2"]),
    ("GPU and memory", ["GPU device", "VRAM budget"]),
]


def function(text, marker):
    start = text.index(marker)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated: " + marker)


def rows_of(menu):
    """The rows table's entries in order: (tab, name, value expression, heading)"""
    table = menu[menu.index("Row rows[] = {"):]
    table = table[:table.index("\n};")]
    out = []
    for m in re.finditer(r'\n    \{(TAB_[A-Z]+), "([^"]+)", ([^,]+),', table):
        entry = table[m.start():table.find("\n    {", m.end())]
        heading = bool(re.search(r"nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true\}", entry))
        out.append((m.group(1), m.group(2), m.group(3).strip(), heading))
    return out


def main():
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    rows = rows_of(menu)
    if len(rows) < 40:
        raise SystemExit("FAIL: the rows table was not found (%d rows read)" % len(rows))

    # the values row_shown can hide: a section needs a row that is not one of them
    hidable = set(re.findall(r"&opts\.\w+", function(menu, "bool row_shown(int i) {")))
    if "&opts.reflex" not in hidable or "&opts.hdr_white" not in hidable:
        raise SystemExit("FAIL: row_shown no longer reads as expected (Reflex, HDR's levels)")

    tabs = {}
    for tab, name, value, heading in rows:
        tabs.setdefault(tab, []).append((name, value, heading))
    for tab, entries in tabs.items():
        for i, (name, value, heading) in enumerate(entries):
            if not heading:
                continue
            section = []
            for later in entries[i + 1:]:
                if later[2]:
                    break
                section.append(later)
            if not section:
                raise SystemExit("FAIL: %s: the section %r has no rows" % (tab, name))
            if all(v in hidable for _, v, _ in section):
                raise SystemExit("FAIL: %s: every row of the section %r can be hidden (row_shown): a GPU without them "
                                 "would show its title over nothing" % (tab, name))

    graphics = tabs.get("TAB_GRAPHICS", [])
    if not graphics or graphics[0][0] != "Quality preset" or graphics[0][2]:
        raise SystemExit("FAIL: the Graphics tab does not start with the Quality preset")
    for tab, expected in (("TAB_GRAPHICS", GRAPHICS), ("TAB_PERFORMANCE", PERFORMANCE)):
        sections = []
        for name, _, heading in tabs[tab]:
            if heading:
                sections.append((name, []))
            elif sections:
                sections[-1][1].append(name)
        if sections != expected:
            raise SystemExit(f"FAIL: {tab} sections are {sections!r}, expected {expected!r}")
    if any(name == "FPS counter" for name, _, _ in tabs["TAB_GAMEPLAY"]):
        raise SystemExit("FAIL: the overlay belongs in Interface")
    if not any(name == "FPS counter" for name, _, _ in tabs["TAB_INTERFACE"]):
        raise SystemExit("FAIL: the Interface tab is missing the overlay")

    # the heading rows are drawn as titles and stepped over by the keys (no code of their own per tab)
    draw = function(menu, "void draw_options(ui::Ctx& screen) {")
    if "if (r.heading) {" not in draw:
        raise SystemExit("FAIL: draw_options no longer draws heading rows as titles")
    tick = function(menu, "void menu_tick() {")
    if "while (heading_at(selected));" not in tick:
        raise SystemExit("FAIL: menu_tick's keys no longer step over heading rows")
    print("menu sections: OK (%d headings)" % sum(r[3] for r in rows))


if __name__ == "__main__":
    main()
