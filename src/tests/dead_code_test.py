#!/usr/bin/env python3
"""Unit tests for the dead-code analysis on synthetic readelf, nm and link inputs."""

import runpy
import tempfile
from pathlib import Path

if not __debug__:
    raise SystemExit("assertions are disabled (PYTHONOPTIMIZE); these checks would not run")

ROOT = Path(__file__).resolve().parents[2]
dead_code = runpy.run_path(str(ROOT / "scripts/dead_code.py"))
LinkContext = dead_code["LinkContext"]


def obj(source, sections, symbols, relocations, locations):
    """Format one object the way `readelf -W -S -s -r` and `nm -l` print it.

    sections: [(name, type, flags)], indexed from 1; RELA entries name their
    target index in flags as "I:<index>". symbols: [(name, kind, bind, ndx)].
    relocations: {RELA section index: [(symbol index, addend[, offset])]}.
    """
    lines = ["Section Headers:",
             "  [ 0]                   NULL            00000000 000000 000000 00      0   0  0"]
    offsets = {}
    for index, (name, kind, flags) in enumerate(sections, 1):
        info = 0
        if kind == "RELA":
            flags, info = "I", int(flags.split(":")[1])
            offsets[index] = (name, 0x1000 + index * 0x40)
        lines.append(f"  [{index:2}] {name} {kind:<15} 00000000 {0x1000 + index * 0x40:06x} "
                     f"000018 00 {flags:>3}  0 {info:3}  4")
    for target, entries in relocations.items():
        name, offset = offsets[target]
        lines += ["", f"Relocation section '{name}' at offset 0x{offset:x} contains "
                      f"{len(entries)} entries:",
                  " Offset     Info    Type                Sym. Value  Symbol's Name + Addend"]
        for symbol, addend, *offset in entries:
            lines.append(f"{(offset or [0])[0]:08x}  {symbol << 8 | 1:08x} R_XTENSA_32"
                         f"            00000000   {symbols[symbol - 1][0]} + {addend:x}")
    lines += ["", f"Symbol table '.symtab' contains {len(symbols) + 1} entries:",
              "   Num:    Value  Size Type    Bind   Vis      Ndx Name",
              "     0: 00000000     0 NOTYPE  LOCAL  DEFAULT  UND "]
    for index, (name, kind, bind, ndx) in enumerate(symbols, 1):
        lines.append(f"{index:6}: 00000000    16 {kind:<7} {bind:<6} DEFAULT {ndx:>4} {name}")
    parsed = dead_code["parse_readelf"](source, "\n".join(lines) + "\n")
    dead_code["parse_locations"](parsed, "".join(
        f"00000000 T {name}\t{ROOT}/{location}\n" for name, location in locations.items()))
    return parsed


def dead(*builds):
    return [name for _, name in dead_code["dead_functions"](
        [dead_code["analyze"](objects, link) for objects, link in builds])]


# One firmware object: a caller, a callee reached by a call, a handler reached
# only through a function pointer table, a virtual function reached from its
# vtable, an SDK entry point, and a function that only calls itself.
firmware = obj("src/a.cpp", [
    (".text._Z6callerv", "PROGBITS", "AX"),          # 1
    (".literal._Z6callerv", "PROGBITS", "AX"),       # 2
    (".rela.literal._Z6callerv", "RELA", "I:2"),     # 3
    (".text._Z6calleev", "PROGBITS", "AX"),          # 4
    (".text._ZL7handlerv", "PROGBITS", "AX"),        # 5
    (".rodata._ZL5table", "PROGBITS", "A"),          # 6
    (".rela.rodata._ZL5table", "RELA", "I:6"),       # 7
    (".text._ZN3Foo3runEv", "PROGBITS", "AXG"),      # 8
    (".rodata._ZTV3Foo", "PROGBITS", "AG"),          # 9
    (".rela.rodata._ZTV3Foo", "RELA", "I:9"),        # 10
    (".text.app_main", "PROGBITS", "AX"),            # 11
    (".text._Z7recursei", "PROGBITS", "AX"),         # 12
    (".literal._Z7recursei", "PROGBITS", "AX"),      # 13
    (".rela.literal._Z7recursei", "RELA", "I:13"),   # 14
    (".debug_info", "PROGBITS", ""),                 # 15
    (".rela.debug_info", "RELA", "I:15"),            # 16
], [
    ("_Z6callerv", "FUNC", "GLOBAL", "1"),           # 1
    ("_Z6calleev", "FUNC", "GLOBAL", "4"),           # 2
    (".text._ZL7handlerv", "SECTION", "LOCAL", "5"), # 3
    ("_ZL7handlerv", "FUNC", "LOCAL", "5"),          # 4
    ("_ZN3Foo3runEv", "FUNC", "WEAK", "8"),          # 5
    ("app_main", "FUNC", "GLOBAL", "11"),            # 6
    ("_Z7recursei", "FUNC", "GLOBAL", "12"),         # 7
    ("_Z6unusedv", "FUNC", "GLOBAL", "UND"),         # 8
], {3: [(2, 0)], 7: [(3, 0)], 10: [(5, 0)], 14: [(7, 0)], 16: [(1, 0), (7, 0)]}, {
    "_Z6callerv": "src/a.cpp:1", "_Z6calleev": "src/a.cpp:5", "_ZL7handlerv": "src/a.cpp:9",
    "_ZN3Foo3runEv": "src/foo.h:3", "app_main": "idf/main/main.cpp:2",
    "_Z7recursei": "src/a.cpp:20"})
sdk = LinkContext()
dead_code["parse_nm"]("esp-idf/freertos/libfreertos.a[app_startup.c.obj]: app_main U\n"
                      "esp-idf/freertos/libfreertos.a[port.c.obj]: vPortTick T 0 10\n", sdk)
assert sdk.undefined == {"app_main"} and not sdk.weak
# Live via call, function pointer, vtable and SDK undefined symbol; recursion
# and debug-information references do not make a function live. The caller
# itself has no user in this build.
assert dead(([firmware], sdk)) == ["_Z6callerv", "_Z7recursei"], dead(([firmware], sdk))
assert dead(([firmware], LinkContext())) == ["app_main", "_Z6callerv", "_Z7recursei"]

# Linker roots (-u), weak SDK definitions (overrides), --wrap and KEEP
# sections also make a function live.
assert dead(([firmware], LinkContext(roots={"_Z6callerv", "_Z7recursei", "app_main"}))) == []
assert dead(([firmware], LinkContext(weak={"app_main"}))) == ["_Z6callerv", "_Z7recursei"]
assert dead(([firmware], LinkContext(keep={".text._Z7recurse*", ".text._Z6caller*"},
                                     undefined={"app_main"}))) == []
wrapped = obj("src/log.cpp", [(".text.__wrap_esp_log_write", "PROGBITS", "AX")],
              [("__wrap_esp_log_write", "FUNC", "GLOBAL", "1")], {},
              {"__wrap_esp_log_write": "src/log.cpp:4"})
assert dead(([wrapped], LinkContext(undefined={"esp_log_write"}))) == ["__wrap_esp_log_write"]
assert dead(([wrapped], LinkContext(undefined={"esp_log_write"},
                                    wrapped={"esp_log_write"}))) == []

# A function compiled into only one variant is dead only when no variant uses
# it; one used by another variant is live.
variant_a = obj("src/v.cpp", [(".text._Z4onlyv", "PROGBITS", "AX"),
                              (".text._Z6sharedv", "PROGBITS", "AX")],
                [("_Z4onlyv", "FUNC", "GLOBAL", "1"), ("_Z6sharedv", "FUNC", "GLOBAL", "2")],
                {}, {"_Z4onlyv": "src/v.cpp:1", "_Z6sharedv": "src/v.cpp:3"})
variant_b = obj("src/w.cpp", [(".text._Z4userv", "PROGBITS", "AX"),
                              (".literal._Z4userv", "PROGBITS", "AX"),
                              (".rela.literal._Z4userv", "RELA", "I:2")],
                [("_Z4userv", "FUNC", "GLOBAL", "1"), ("_Z6sharedv", "FUNC", "GLOBAL", "UND")],
                {3: [(2, 0)]}, {"_Z4userv": "src/w.cpp:1"})
roots = LinkContext(roots={"_Z4userv"})
assert dead(([variant_a], LinkContext()), ([variant_a, variant_b], roots)) == ["_Z4onlyv"]
# One name defined by two variants' own sources is reported at both places.
variant_c = obj("src/x.cpp", [(".text._Z4onlyv", "PROGBITS", "AX")],
                [("_Z4onlyv", "FUNC", "GLOBAL", "1")], {}, {"_Z4onlyv": "src/x.cpp:9"})
assert [location.removeprefix(f"{ROOT}/") for location, _ in dead_code["dead_functions"](
    [dead_code["analyze"]([variant_a], LinkContext()),
     dead_code["analyze"]([variant_c], LinkContext())])] == [
    "src/v.cpp:1", "src/v.cpp:3", "src/x.cpp:9"]

# An inline function from a project header, emitted in two objects and called
# by neither, is reported once; an unused inline function from an SDK header
# is outside the project and not reported. Aliases (complete and base object
# constructors) share liveness, and clone suffixes belong to their function.
inline = [obj(f"src/{name}.cpp",
              [(".text._ZN6Helper4idleEv", "PROGBITS", "AXG"),
               (".text.sdk_inline", "PROGBITS", "AXG"),
               (".text._ZN3BarC2Ev", "PROGBITS", "AX"),
               (".text._Z4bodyv", "PROGBITS", "AX"),
               (".literal._Z4bodyv", "PROGBITS", "AX"),
               (".rela.literal._Z4bodyv", "RELA", "I:5"),
               (".text._Z5splitv$constprop$0", "PROGBITS", "AX"),
               (".text._Z5splitv", "PROGBITS", "AX")],
              [("_ZN6Helper4idleEv", "FUNC", "WEAK", "1"), ("sdk_inline", "FUNC", "WEAK", "2"),
               ("_ZN3BarC2Ev", "FUNC", "GLOBAL", "3"), ("_ZN3BarC1Ev", "FUNC", "GLOBAL", "3"),
               ("_Z4bodyv", "FUNC", "GLOBAL", "4"),
               ("_Z5splitv$constprop$0", "FUNC", "LOCAL", "7"),
               ("_Z5splitv", "FUNC", "GLOBAL", "8")],
              {6: [(4, 0), (6, 0)]},
              {"_ZN6Helper4idleEv": "src/Helper.h:7", "_ZN3BarC2Ev": "src/bar.cpp:2",
               "_ZN3BarC1Ev": "src/bar.cpp:2", "_Z4bodyv": f"src/{name}.cpp:1",
               "_Z5splitv$constprop$0": "src/split.cpp:4", "_Z5splitv": "src/split.cpp:4",
               "sdk_inline": "../esp/esp-idf/components/hal/include/x.h:3"})
          for name in ("one", "two")]
assert dead((inline, LinkContext(roots={"_Z4bodyv"}))) == [
    "_ZN6Helper4idleEv"], dead((inline, LinkContext(roots={"_Z4bodyv"})))

# The link command and linker scripts provide roots, wraps, KEEP sections and
# the non-project inputs; project archives are analyzed as objects instead.
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-dead-code-") as temporary:
    build = Path(temporary)
    for path in ("esp-idf/freertos/libfreertos.a", "esp-idf/openBrewByWeight/libopenBrewByWeight.a",
                 "lib/libbar.a", "CMakeFiles/app.elf.dir/project_elf_src.c.obj"):
        (build / path).parent.mkdir(parents=True, exist_ok=True)
        (build / path).write_bytes(b"")
    (build / "ldflags").write_text("-nostartfiles -Wl,--undefined=from_flags\n")
    (build / "lib/sections.ld").write_text(
        "ENTRY(call_start)\nINCLUDE extra.ld\n/* KEEP(*(.commented)) */\n"
        "SECTIONS { .x : { KEEP(*(SORT_BY_INIT_PRIORITY(EXCLUDE_FILE(*crtend.*) .ctors.*)))\n"
        "KEEP(*(.vectors.text .boot.*)) } }\n")
    (build / "lib/extra.ld").write_text("EXTERN(from_script other_root)\n")
    (build / "build.ninja").write_text(
        "build app.elf: LINK CMakeFiles/app.elf.dir/project_elf_src.c.obj | x\n"
        f'  LINK_FLAGS = @"{build}/ldflags" -Wl,--wrap=esp_log_write -T sections.ld\n'
        f"  LINK_PATH = -L{build}/lib\n"
        "  LINK_LIBRARIES = esp-idf/freertos/libfreertos.a "
        "esp-idf/openBrewByWeight/libopenBrewByWeight.a -u app_main -lbar\n")
    link, inputs = dead_code["link_context"](
        build, {"CMAKE_PROJECT_NAME": "app", "CMAKE_NM": "/nonexistent/nm"},
        ["main", "openBrewByWeight"])
    assert link.roots == {"app_main", "from_flags", "call_start", "from_script",
                          "other_root"}, link.roots
    assert link.wrapped == {"esp_log_write"} and link.keep == {".ctors.*", ".vectors.text",
                                                              ".boot.*"}, link
    assert inputs == [build / "esp-idf/freertos/libfreertos.a",
                      build / "CMakeFiles/app.elf.dir/project_elf_src.c.obj",
                      build / "lib/libbar.a"], inputs

# Compiler-written bodies (implicit or `= default` members, lambda conversion
# operators) are not candidates: DWARF marks their declaration artificial or
# defaulted, and the .debug_info relocation of the copy's DW_AT_low_pc names it.
generated = obj("src/g.cpp", [(".text._ZN1SC2Ev", "PROGBITS", "AXG"),
                              (".text._ZN1TC2Ev", "PROGBITS", "AXG"),
                              (".text._Z4userv", "PROGBITS", "AX"),
                              (".debug_info", "PROGBITS", ""),
                              (".rela.debug_info", "RELA", "I:4")],
                [("_ZN1SC2Ev", "FUNC", "WEAK", "1"), ("_ZN1TC2Ev", "FUNC", "WEAK", "2"),
                 ("_Z4userv", "FUNC", "GLOBAL", "3")],
                {5: [(1, 0, 0x75), (2, 0, 0x95), (3, 0, 0xb5)]},
                {"_ZN1SC2Ev": "src/g.cpp:1", "_ZN1TC2Ev": "src/g.cpp:2", "_Z4userv": "src/g.cpp:3"})
dead_code["parse_artificial"](generated, """
 <1><54>: Abbrev Number: 16 (DW_TAG_subprogram)
    <55>   DW_AT_name        : S
    <5b>   DW_AT_artificial  : 1
 <2><5f>: Abbrev Number: 1 (DW_TAG_formal_parameter)
    <64>   DW_AT_artificial  : 1
 <1><60>: Abbrev Number: 16 (DW_TAG_subprogram)
    <61>   DW_AT_linkage_name: (indirect string, offset: 0x8a): _ZN1TC4Ev
    <65>   DW_AT_defaulted   : 1	(in class)
 <1><70>: Abbrev Number: 3 (DW_TAG_subprogram)
    <71>   DW_AT_specification: <0x54>
    <75>   DW_AT_low_pc      : 0
 <1><80>: Abbrev Number: 3 (DW_TAG_subprogram)
    <81>   DW_AT_abstract_origin: <0x60>
    <95>   DW_AT_low_pc      : 0
 <1><a0>: Abbrev Number: 3 (DW_TAG_subprogram)
    <a1>   DW_AT_name        : user
 <2><a8>: Abbrev Number: 1 (DW_TAG_formal_parameter)
    <a9>   DW_AT_artificial  : 1
 <1><b0>: Abbrev Number: 3 (DW_TAG_subprogram)
    <b1>   DW_AT_specification: <0xa0>
    <b5>   DW_AT_low_pc      : 0
""")
assert generated.artificial == {0x75, 0x95}, generated.artificial
assert dead(([generated], LinkContext())) == ["_Z4userv"]

# A constructor named through its complete-object symbol in another object
# keeps its base-object alias alive.
constructed = obj("src/c.cpp", [(".text._ZN3BazC2Ev", "PROGBITS", "AX")],
                  [("_ZN3BazC2Ev", "FUNC", "GLOBAL", "1"), ("_ZN3BazC1Ev", "FUNC", "GLOBAL", "1")],
                  {}, {"_ZN3BazC2Ev": "src/c.cpp:1", "_ZN3BazC1Ev": "src/c.cpp:1"})
constructor_user = obj("src/d.cpp", [(".text._Z4makev", "PROGBITS", "AX"),
                                     (".literal._Z4makev", "PROGBITS", "AX"),
                                     (".rela.literal._Z4makev", "RELA", "I:2")],
                       [("_Z4makev", "FUNC", "GLOBAL", "1"), ("_ZN3BazC1Ev", "FUNC", "GLOBAL", "UND")],
                       {3: [(2, 0)]}, {"_Z4makev": "src/d.cpp:1"})
assert dead(([constructed, constructor_user], LinkContext(roots={"_Z4makev"}))) == []

# A constexpr function evaluated only in constant expressions leaves no
# reference in any object; naming it elsewhere in firmware sources is its use.
header = f"{ROOT}/src/k.h"
sources = {header: "template <size_t N>\n"
                   "constexpr bool tableOk(const int (&t)[N]) {\n  return N > 0;\n}\n"
                   "static_assert(tableOk(kTable));\n"
                   "constexpr int unusedHelper() { return 1; }\n"
                   "inline int plain() { return 2; }\nint caller() { return plain(); }\n"}
sources[f"{ROOT}/src/m.h"] = "constexpr int unusedHelper() { return 2; }\n"
definitions = {f"{header}:2", f"{header}:6", f"{header}:7", f"{header}:8", f"{ROOT}/src/m.h:1"}
compile_time_used = dead_code["compile_time_used"]
assert compile_time_used(f"{header}:2", "bool tableOk<3u>(int const (&) [3u])", sources,
                         definitions)
# Another variant's definition of the same name is not a use.
assert not compile_time_used(f"{header}:6", "unusedHelper()", sources, definitions)
assert not compile_time_used(f"{header}:7", "plain()", sources, definitions)
assert dead_code["unqualified"]("unsigned int ns::groupBytes<26u>(unsigned char, "
                                "ns::Field const (&) [26u])") == "groupBytes"

print("dead-code analysis tests: OK")
