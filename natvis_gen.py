#!/usr/bin/env python3
"""
natvis_gen.py - generate a Visual Studio .natvis visualizer from C structs.

Parses C source with tree-sitter, resolves typedefs, and emits a valid,
XML-escaped .natvis file with one <Type> per struct/union.

SELF-CONTAINED. The only dependencies are two pip packages:
    python -m pip install tree-sitter tree-sitter-c

WHAT IS AUTOMATIC
    - every field, with its real name and type
    - typedef resolution, so `typedef struct blk {...} blk_t;` matches correctly
    - nested structs/unions pulled in recursively
    - char* rendered as strings, other pointers as addresses
    - arrays given an ArrayItems expansion
    - a DisplayString summarising the first few scalar fields

WHAT IS *NOT* AUTOMATIC  (and cannot be)
    Allocator semantics. Nothing in the type `size_t size_and_flags` tells a
    parser that bit 0 is an in-use flag or that the size is masked with ~7.
    That lives in your malloc.c, not your struct. With --allocator this script
    GUESSES from field names and emits an <Intrinsic> block marked
    CHECK-THIS. Confirm every guess. A visualizer that is confidently wrong is
    worse than none, because you will trust it.

USAGE
    python natvis_gen.py mem_op.h -o alloc.natvis
    python natvis_gen.py mem_op.h --struct block_hdr --allocator -o alloc.natvis
    python natvis_gen.py src/*.h --all -o types.natvis
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, quoteattr

# --- self-contained C parser (tree-sitter; no external script needed) -------
try:
    from tree_sitter import Language, Parser
    import tree_sitter_c
except ImportError:
    sys.exit(
        "error: missing dependencies.\n"
        "    python -m pip install tree-sitter tree-sitter-c"
    )

_LANG = Language(tree_sitter_c.language())
_PARSER = Parser(_LANG)


def _txt(node, src: bytes) -> str:
    return src[node.start_byte:node.end_byte].decode("utf8", "replace")


def _norm(s: str) -> str:
    """Collapse whitespace and tidy pointer spacing: 'struct  x*' -> 'struct x *'."""
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r"\s*\*", " *", s)
    return re.sub(r"\s+", " ", s).strip()


def _decl_name_and_type(decl, src: bytes, base: str):
    """
    Walk a declarator down to its identifier, accumulating the decoration
    (pointers, arrays, function-pointer parens) into a type string.

    tree-sitter gives us the declarator as a nested tree, e.g. for
    `struct block_hdr *next_free` the pointer_declarator wraps the identifier.
    We rebuild the type by taking the full declarator text and removing the
    identifier from it, which handles arrays and function pointers correctly
    without special-casing each shape.
    """
    ident = None
    stack = [decl]
    while stack:
        n = stack.pop()
        if n.type in ("identifier", "field_identifier", "type_identifier"):
            ident = n
            break
        stack.extend(reversed(n.children))
    if ident is None:
        return None, None

    whole = _txt(decl, src)
    name = _txt(ident, src)
    # Remove just the identifier occurrence, keeping the decoration around it.
    off = ident.start_byte - decl.start_byte
    decorated = whole[:off] + whole[off + len(name):]
    return name, _norm(base + " " + decorated)


def _fields_of(body, src: bytes):
    """Extract (name, type) for every named field in a struct/union body."""
    out = []
    for fd in body.named_children:
        if fd.type != "field_declaration":
            continue
        # The base type is everything before the first declarator.
        decls = [c for c in fd.children
                 if c.type.endswith("declarator") or c.type == "field_identifier"]
        if not decls:
            continue
        base = _norm(src[fd.start_byte:decls[0].start_byte].decode("utf8", "replace"))
        for d in decls:
            if d.type == "field_identifier":
                out.append((_txt(d, src), base))
            else:
                n, t = _decl_name_and_type(d, src, base)
                if n:
                    out.append((n, t))
    return out


def _walk_records(node, src: bytes, path: str, records: list, typedefs: dict):
    """Find every struct/union (tagged, anonymous, or behind a typedef)."""
    for n in node.named_children:

        if n.type == "type_definition":
            # typedef ...
            alias = None
            for c in n.named_children:
                if c.type in ("type_identifier", "primitive_type") and c.prev_sibling:
                    alias = _txt(c, src)
            spec = next((c for c in n.named_children
                         if c.type in ("struct_specifier", "union_specifier")), None)
            if spec is not None:
                body = spec.child_by_field_name("body")
                tagn = spec.child_by_field_name("name")
                tag = _txt(tagn, src) if tagn else None
                if body is not None:
                    flds = _fields_of(body, src)
                    if flds:
                        kind = "struct" if spec.type == "struct_specifier" else "union"
                        records.append(Record(kind, tag, alias, flds, path,
                                              n.start_point[0] + 1))
                    _walk_records(body, src, path, records, typedefs)
            else:
                # simple alias: typedef unsigned long ulong_t;
                decl = n.named_children[-1] if n.named_children else None
                if alias and decl is not None:
                    under = _norm(src[n.start_byte:decl.start_byte]
                                  .decode("utf8", "replace").replace("typedef", "", 1))
                    if under:
                        typedefs[alias] = base_type(under)
            continue

        if n.type in ("struct_specifier", "union_specifier"):
            body = n.child_by_field_name("body")
            tagn = n.child_by_field_name("name")
            if body is not None and tagn is not None:
                flds = _fields_of(body, src)
                if flds:
                    kind = "struct" if n.type == "struct_specifier" else "union"
                    records.append(Record(kind, _txt(tagn, src), None, flds, path,
                                          n.start_point[0] + 1))
                _walk_records(body, src, path, records, typedefs)
            continue

        _walk_records(n, src, path, records, typedefs)


# ---------------------------------------------------------------------------
# Type-string analysis
#
# Field types arrive as reconstructed source strings: "size_t",
# "struct block_hdr *", "char [32]", "const void *", "int (*)(void *)".
# We need to pick these apart to decide how to render each field.
# ---------------------------------------------------------------------------

QUALIFIERS = {"const", "volatile", "restrict", "_Atomic", "static", "register"}
RECORD_KEYWORDS = {"struct", "union", "enum"}


def ptr_depth(t: str) -> int:
    """Number of '*' in the type. Ignores '*' inside (...) of a func-pointer."""
    return t.replace(func_ptr_args(t), "").count("*") if is_func_ptr(t) else t.count("*")


def is_func_ptr(t: str) -> bool:
    return bool(re.search(r"\(\s*\*\s*\)", t)) or bool(re.search(r"\(\*[^)]*\)\s*\(", t))


def func_ptr_args(t: str) -> str:
    m = re.search(r"\([^()]*\)\s*$", t)
    return m.group(0) if m else ""


def array_dims(t: str):
    """['32'] for 'char [32]', ['3','3'] for 'int [3][3]', [] if not an array."""
    return re.findall(r"\[\s*([^\]]*?)\s*\]", t)


def is_array(t: str) -> bool:
    return bool(array_dims(t)) and not is_func_ptr(t)


def base_type(t: str) -> str:
    """
    Strip qualifiers, pointers, arrays and the struct/union/enum keyword to get
    the bare type name.  'const struct block_hdr **' -> 'block_hdr'
    """
    t = re.sub(r"\[[^\]]*\]", " ", t)          # arrays
    t = t.replace("*", " ")                    # pointers
    words = [w for w in re.split(r"[\s()]+", t) if w]
    words = [w for w in words if w not in QUALIFIERS and w not in RECORD_KEYWORDS]
    return words[-1] if words else ""


INTEGERISH = re.compile(
    r"^(_Bool|bool|char|signed|unsigned|short|int|long|size_t|ssize_t|ptrdiff_t|"
    r"intptr_t|uintptr_t|u?int(8|16|32|64)_t|u?intmax_t)$"
)


def is_scalar(t: str) -> bool:
    """Plain integer-ish value: no pointer, no array, no func-ptr."""
    if ptr_depth(t) or is_array(t) or is_func_ptr(t):
        return False
    return bool(INTEGERISH.match(base_type(t))) or base_type(t) in ("float", "double")


def is_c_string(t: str) -> bool:
    """char* / const char* -- worth rendering with the ,s specifier."""
    return ptr_depth(t) == 1 and base_type(t) == "char" and not is_array(t)


def is_char_array(t: str) -> bool:
    return is_array(t) and base_type(t) == "char"


# ---------------------------------------------------------------------------
# Harvest records from the parse tree
# ---------------------------------------------------------------------------

class Record:
    """A struct or union we will emit a <Type> for."""

    def __init__(self, kind, tag, typedef_name, fields, src_file, line):
        self.kind = kind                  # 'struct' | 'union'
        self.tag = tag                    # may be None (anonymous)
        self.typedef_name = typedef_name  # may be None
        self.fields = fields              # [(name, type)]
        self.src_file = src_file
        self.line = line

    @property
    def natvis_name(self):
        """
        The string that must go in <Type Name="...">.

        For C, the debugger names a tagged struct by its TAG. An anonymous
        struct behind a typedef is named by the TYPEDEF. Getting this wrong
        means the visualizer silently never fires -- which is the single most
        common natvis failure, and it looks identical to 'natvis is broken'.

        If it doesn't fire: put a variable of the type in the Watch window and
        read the Type column. That string is authoritative; this heuristic is not.
        """
        return self.tag or self.typedef_name

    @property
    def all_names(self):
        return [n for n in (self.tag, self.typedef_name) if n]


def parse_files(paths):
    records, typedefs = [], {}
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        tree = _PARSER.parse(data)
        if tree.root_node.has_error:
            print(f"warning: syntax errors in {p}; extraction may be partial",
                  file=sys.stderr)
        _walk_records(tree.root_node, data, p, records, typedefs)
    return records, typedefs


def resolve(name, typedef_map, depth=8):
    """Follow simple typedef aliases to a concrete name."""
    seen = set()
    while name in typedef_map and name not in seen and depth:
        seen.add(name)
        name = typedef_map[name]
        depth -= 1
    return name


# ---------------------------------------------------------------------------
# Allocator heuristics  -- GUESSES, and labelled as such in the output
# ---------------------------------------------------------------------------

SIZE_HINT = re.compile(r"size|len|bytes|words|sz\b", re.I)
FLAG_HINT = re.compile(r"flag|used|alloc|inuse|in_use|free|state|status", re.I)
NEXT_HINT = re.compile(r"next|fwd|succ|fd\b", re.I)


def guess_roles(rec):
    """Return (size_field, flag_field, next_field) -- any may be None."""
    size_f = flag_f = next_f = None
    for name, t in rec.fields:
        if next_f is None and ptr_depth(t) >= 1 and NEXT_HINT.search(name):
            next_f = name
        if size_f is None and is_scalar(t) and SIZE_HINT.search(name):
            size_f = name
        if flag_f is None and is_scalar(t) and FLAG_HINT.search(name):
            flag_f = name
    # A packed size+flags word satisfies both hints at once.
    if size_f and not flag_f and FLAG_HINT.search(size_f):
        flag_f = size_f
    return size_f, flag_f, next_f


# ---------------------------------------------------------------------------
# Emit natvis
#
# EVERY string that lands in XML goes through escape()/quoteattr(). A bare '&'
# from a bitwise-AND is a hard parse error and natvis dies SILENTLY.
# ---------------------------------------------------------------------------


def xml_comment_safe(s: str) -> str:
    """
    XML comments may not contain '--' (and may not end with '-'). A stray double
    hyphen in prose produces a file that dies at parse time, i.e. exactly the
    silent-failure mode this tool exists to prevent. Neutralise it centrally.
    """
    s = s.replace("--", "-")
    return s[:-1] + "- " if s.endswith("-") else s


def field_item(name, type_str, indent="      "):
    disp = f"[{name}]"
    if is_c_string(type_str):
        expr = f"{name},s"          # render char* as a string
    elif is_char_array(type_str):
        expr = f"{name},s"
    elif is_func_ptr(type_str):
        expr = name
    elif is_array(type_str):
        return (f'{indent}<Synthetic Name={quoteattr(disp)}>\n'
                f'{indent}  <DisplayString>{escape(type_str)}</DisplayString>\n'
                f'{indent}  <Expand><ExpandedItem>{escape(name)}</ExpandedItem></Expand>\n'
                f'{indent}</Synthetic>')
    elif ptr_depth(type_str) >= 1:
        expr = name
    else:
        expr = name
    return f'{indent}<Item Name={quoteattr(disp)}>{escape(expr)}</Item>'


def display_string(rec, max_fields=3):
    """One-line summary from the first few scalar fields."""
    picks = [(n, t) for n, t in rec.fields if is_scalar(t) or is_c_string(t)][:max_fields]
    if not picks:
        return f"<DisplayString>{escape(rec.natvis_name)}</DisplayString>"
    parts = []
    for n, t in picks:
        spec = ",s" if is_c_string(t) else ""
        parts.append(f"{n}={{{n}{spec}}}")
    body = " ".join(parts)
    return f"<DisplayString>{escape(body)}</DisplayString>"


class SemanticsError(SystemExit):
    pass


def emit_allocator_intrinsics(rec, opts):
    """
    Build the <Intrinsic> block.

    HARD RULE: never invent a default. Earlier versions emitted Size()=0 and
    IsUsed()=true when the name heuristics missed, which produced a file that
    was syntactically perfect and semantically garbage: every block displayed
    as "[USED] 0 bytes", NextByAddr() pointed at itself, and the free-list walk
    was guarded on a condition that was always false so it never rendered at
    all. A visualizer that lies is worse than no visualizer, because you trust
    it. So: if we cannot determine a value, we REFUSE to emit the file and tell
    the user exactly which flag to pass.
    """
    g_size, g_flag, g_next = guess_roles(rec)

    size_expr = opts.size_expr or (f"{g_size} & ~7" if g_size else None)
    used_expr = opts.used_expr or (f"({g_flag} & 1) != 0" if g_flag else None)
    next_field = opts.next_field or g_next

    missing = []
    if not size_expr:
        missing.append('  --size-expr "<expr>"   e.g. "hdr & ~7"  (block size in BYTES)')
    if not used_expr:
        missing.append('  --used-expr "<expr>"   e.g. "(hdr & 1) != 0"  (true = ALLOCATED)')
    if missing:
        raise SemanticsError(
            "error: --allocator cannot infer the block semantics for "
            f"'{rec.natvis_name}'.\n\n"
            "Nothing in a struct's TYPE says which bit is the in-use flag or how\n"
            "the size is encoded. That lives in your malloc.c. Rather than guess\n"
            "and hand you a visualizer that is confidently wrong, this refuses.\n\n"
            "Supply the semantics yourself:\n" + "\n".join(missing) + "\n"
            '  --next-field <name>    free-list pointer (omit if no free list)\n'
            '  --payload-field <name> if the payload is an explicit MEMBER\n'
            "\nFields available on this struct:\n"
            + "\n".join(f"    {t:28} {n}" for n, t in rec.fields)
            + "\n\nOr drop --allocator entirely for a plain field dump, which is\n"
              "always correct because it invents nothing."
        )

    # Payload: either an explicit member, or the classic "byte after header".
    if opts.payload_field:
        payload_expr = f"({opts.payload_field})"
        payload_note = f"explicit member '{opts.payload_field}'"
    else:
        payload_expr = "(void*)(Self() + 1)"
        payload_note = "assumed to be the byte AFTER the header"

    tag = rec.natvis_name
    first = rec.fields[0][0]
    guessed = []
    if not opts.size_expr and g_size:
        guessed.append("Size")
    if not opts.used_expr and g_flag:
        guessed.append("IsUsed")

    L = ["      <!--"]
    L.append("        ALLOCATOR INTRINSICS")
    if guessed:
        L.append("")
        L.append(f"        {', '.join(guessed)} were GUESSED from field names.")
        L.append("        Verify before trusting a single displayed value:")
        L.append("          ALIGNMENT: mask assumed ~7 (8-byte). 16-byte needs ~15.")
        L.append("          POLARITY:  assumed bit 0 SET == allocated. Many")
        L.append("                     allocators mean the opposite; glibc uses")
        L.append("                     bit 0 for 'PREVIOUS block in use'.")
        L.append("          UNITS:     assumed BYTES, INCLUDING the header.")
        L.append("        Override with --size-expr / --used-expr.")
    else:
        L.append("        Supplied explicitly on the command line.")
    L.append("")
    L.append(f"        Payload: {payload_note}.")
    L.append("")
    L.append("        Sanity check before you rely on any of this: allocate a")
    L.append("        block of a size you know and confirm Size() reports that")
    L.append("        number. If it is off by sizeof(header), fix Size() first.")
    L.append("      -->")
    L.append(f'      <Intrinsic Name="Self" '
             f'Expression={quoteattr(f"(struct {tag}*)&{first}")}/>')
    L.append(f'      <Intrinsic Name="Size" Expression={quoteattr(size_expr)}/>')
    L.append(f'      <Intrinsic Name="IsUsed" Expression={quoteattr(used_expr)}/>')
    L.append(f'      <Intrinsic Name="Payload" Expression={quoteattr(payload_expr)}/>')
    L.append(f'      <Intrinsic Name="NextByAddr" '
             f'Expression={quoteattr(f"(struct {tag}*)((char*)Self() + Size())")}/>')
    if next_field:
        L.append(f'      <Intrinsic Name="Next" Expression={quoteattr(next_field)}/>')
    L.append("")
    L.append('      <DisplayString Condition="IsUsed()">[USED] {Size(),d} bytes @ {Payload()}</DisplayString>')
    L.append('      <DisplayString Condition="!IsUsed()">[FREE] {Size(),d} bytes</DisplayString>')
    return "\n".join(L), next_field



def self_ptr_fields(rec, typedefs):
    """
    Fields that are single pointers back to the SAME record.

    This is STRUCTURAL, not semantic. `struct mem_something *next` inside
    `struct mem_something` is self-referential as a matter of type, so we can
    infer it with certainty -- unlike "which bit is the in-use flag", which is a
    convention living in your malloc.c and which this tool refuses to guess.

    Returns every such field: a doubly-linked node yields both next and prev,
    and each gets its own walk rather than one being silently picked.
    """
    names = set(rec.all_names)
    out = []
    for n, t in rec.fields:
        if ptr_depth(t) == 1 and not is_func_ptr(t):
            if resolve(base_type(t), typedefs) in names or base_type(t) in names:
                out.append(n)
    return out


def emit_custom_walk(rec, field, max_hops, guard=None, indent="      "):
    """
    A cycle-safe chain walk using <CustomListItems>.

    WHY NOT <LinkedListItems>: it has no cycle detection. A free list corrupted
    into a loop is the classic double-free signature -- precisely the bug you
    build this to find -- and LinkedListItems will walk it until the debugger
    gives up. Hanging your debugger is a poor way to report a bug it could have
    named.

    This uses Floyd's tortoise-and-hare: a slow cursor advancing one node per
    step and a fast cursor advancing two. If they ever meet, the chain contains
    a cycle. The hop cap is a second, blunter backstop for the case where the
    chain is merely absurdly long or the pointers are garbage rather than
    cyclic.

    <Exec> is evaluated by the DEBUGGER, not executed in your process. It cannot
    call functions and cannot mutate the debuggee. Walking a corrupt list here
    is safe; it cannot make the corruption worse.
    """
    i = indent
    L = []
    L.append(f'{i}<CustomListItems MaxItemsPerView="500">')
    L.append(f'{i}  <Variable Name="slow" InitialValue="this"/>')
    L.append(f'{i}  <Variable Name="fast" InitialValue="this"/>')
    L.append(f'{i}  <Variable Name="cyclic" InitialValue="false"/>')
    L.append(f'{i}  <Variable Name="hops" InitialValue="0"/>')
    L.append("")
    cond = f"slow != 0 &amp;&amp; hops &lt; {max_hops}"
    if guard:
        cond = f"{escape(guard)} &amp;&amp; " + cond
    L.append(f'{i}  <Loop Condition="{cond}">')
    L.append(f'{i}    <Item>slow</Item>')
    L.append(f'{i}    <Exec>slow = slow-&gt;{escape(field)}</Exec>')
    L.append("")
    L.append(f'{i}    <!-- hare: two hops for every one of the tortoise -->')
    L.append(f'{i}    <If Condition="fast != 0">')
    L.append(f'{i}      <Exec>fast = fast-&gt;{escape(field)}</Exec>')
    L.append(f'{i}    </If>')
    L.append(f'{i}    <If Condition="fast != 0">')
    L.append(f'{i}      <Exec>fast = fast-&gt;{escape(field)}</Exec>')
    L.append(f'{i}    </If>')
    L.append("")
    L.append(f'{i}    <!-- they can only meet if the chain loops -->')
    L.append(f'{i}    <If Condition="fast != 0 &amp;&amp; fast == slow">')
    L.append(f'{i}      <Exec>cyclic = true</Exec>')
    L.append(f'{i}      <Break/>')
    L.append(f'{i}    </If>')
    L.append("")
    L.append(f'{i}    <Exec>hops = hops + 1</Exec>')
    L.append(f'{i}  </Loop>')
    L.append("")
    L.append(f'{i}  <!-- These two only appear when something is WRONG. Seeing')
    L.append(f'{i}       either one is the diagnosis, not a rendering artifact. -->')
    L.append(f'{i}  <Item Name="[!! CYCLE DETECTED - chain loops, list is corrupt !!]" '
             f'Condition="cyclic">slow</Item>')
    L.append(f'{i}  <Item Name="[!! hop cap {max_hops} hit - chain too long or pointers garbage !!]" '
             f'Condition="!cyclic &amp;&amp; hops &gt;= {max_hops}">slow</Item>')
    L.append(f'{i}</CustomListItems>')
    return "\n".join(L)


def emit_type(rec, opts=None, allocator=False):
    L = []
    where = f"{os.path.basename(rec.src_file)}:{rec.line}"
    L.append(f"  <!-- {rec.kind} {rec.natvis_name}   (from {where}) -->")
    L.append(f"  <Type Name={quoteattr(rec.natvis_name)}>")

    next_f = None
    if allocator:
        intr, next_f = emit_allocator_intrinsics(rec, opts)
        L.append(intr)
    else:
        L.append("    " + display_string(rec))

    L.append("    <Expand>")

    # Track display names so a synthesised item can never collide with a real
    # field of the same name (a struct with a member literally called 'payload'
    # produced two <Item Name="[payload]"> entries in an earlier version).
    used_names = set()

    def item(disp, expr):
        d = disp
        if d in used_names:
            d = d[:-1] + " (field)]"
        used_names.add(d)
        return f'      <Item Name={quoteattr(d)}>{escape(expr)}</Item>'

    if allocator:
        L.append(item("[size (bytes)]", "Size(), d"))
        L.append(item("[in use]", "IsUsed()"))
        L.append(item("[payload]", "Payload()"))
        L.append(item("[next by addr]", "NextByAddr()"))

    L.append("      <!-- raw fields, exactly as declared -->")
    for name, t in rec.fields:
        disp = f"[{name}]"
        if disp in used_names:
            disp = f"[{name} (field)]"
        used_names.add(disp)
        if is_c_string(t) or is_char_array(t):
            L.append(f'      <Item Name={quoteattr(disp)}>{escape(name + ",s")}</Item>')
        elif is_array(t):
            L.append(f'      <Synthetic Name={quoteattr(disp)}>')
            L.append(f'        <DisplayString>{escape(t)}</DisplayString>')
            L.append(f'        <Expand><ExpandedItem>{escape(name)}</ExpandedItem></Expand>')
            L.append(f'      </Synthetic>')
        else:
            L.append(f'      <Item Name={quoteattr(disp)}>{escape(name)}</Item>')

    # Chain walks. Every self-referential pointer field gets one, detected
    # structurally from the type -- no field names typed by hand, no guessing.
    chains = self_ptr_fields(rec, opts.typedefs if opts else {})
    if allocator and next_f and next_f not in chains:
        chains.append(next_f)

    max_hops = getattr(opts, "max_hops", 1000) if opts else 1000

    for fld in chains:
        L.append("")
        # Guard the walk when we know the pointer is only valid on free blocks:
        # in a boundary-tag allocator the next pointer overlaps the user payload,
        # so on an ALLOCATED block it holds whatever the caller wrote. Without a
        # flag we cannot know that, so we do not guard -- and say so.
        guard = "!IsUsed()" if (allocator and fld == next_f) else None
        L.append(f'      <Synthetic Name="[chain via {escape(fld)}]">')
        if guard:
            L.append('        <DisplayString Condition="IsUsed()">'
                     '(in use - pointer overlaps payload, not walked)</DisplayString>')
        L.append(f'        <DisplayString>walk {escape(fld)} (cycle-checked)</DisplayString>')
        L.append("        <Expand>")
        L.append(emit_custom_walk(rec, fld, max_hops, guard, indent="          "))
        L.append("        </Expand>")
        L.append("      </Synthetic>")

    L.append("")
    L.append("      <!-- Escape hatch. Keep it: when the visualizer lies to you,")
    L.append("           this is how you find out. -->")
    L.append('      <Item Name="[raw view]">*this,!</Item>')
    L.append("    </Expand>")
    L.append("  </Type>")
    return "\n".join(L)


HEADER = """<?xml version="1.0" encoding="utf-8"?>
<!--
  GENERATED by natvis_gen.py. Edit freely; re-running overwrites.

  INSTALL
    Add to the project (Add > Existing Item), or drop in
      %USERPROFILE%\\Documents\\Visual Studio 2022\\Visualizers\\
    Natvis reloads on every BREAK, so edit + step to see changes.
    No rebuild needed.

  TURN THIS ON BEFORE YOU DEBUG ANYTHING
    Tools > Options > Debugging > Output Window >
        "Natvis diagnostic messages"  ->  Warning
    Tools > Options > Debugging > "Enable Natvis support..."  (on)

    Natvis fails SILENTLY. One bad expression and the entire <Type> is
    skipped with no message, which looks exactly like "natvis isn't
    loading". Without diagnostics on you will debug the wrong thing.

  IF A VISUALIZER NEVER FIRES
    The <Type Name="..."> must match what the DEBUGGER calls the type,
    which for C is the struct TAG (or the typedef name for an anonymous
    struct). Put a variable in the Watch window and read the Type column.
    That string is authoritative. This generator's guess is not.
-->
<AutoVisualizer xmlns="http://schemas.microsoft.com/vstudio/debugger/natvis/2010">
"""

FOOTER = "</AutoVisualizer>\n"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate a .natvis visualizer from C structs.")
    ap.add_argument("files", nargs="+", help=".c/.h files to parse")
    ap.add_argument("-o", "--out", default="generated.natvis")
    ap.add_argument("--struct", action="append", default=[],
                    help="only this struct (repeatable). Default: all.")
    ap.add_argument("--all", action="store_true",
                    help="emit every struct found, including nested ones")
    ap.add_argument("--allocator", action="store_true",
                    help="add allocator intrinsics (Size/IsUsed/Payload/Next)")
    ap.add_argument("--size-expr", metavar="EXPR",
                    help='block size in BYTES, e.g. "hdr & ~7". You supply this; '
                         'it cannot be inferred from the type.')
    ap.add_argument("--used-expr", metavar="EXPR",
                    help='true when ALLOCATED, e.g. "(hdr & 1) != 0". Mind the polarity.')
    ap.add_argument("--next-field", metavar="NAME",
                    help="free-list pointer field (enables the linked-list walk)")
    ap.add_argument("--max-hops", type=int, default=1000, metavar="N",
                    help="cap the chain walk at N nodes (default 1000). A walk "
                         "that hits the cap is itself a bug report.")
    ap.add_argument("--payload-field", metavar="NAME",
                    help="payload is an explicit MEMBER of this name; otherwise the "
                         "payload is assumed to be the byte after the header")
    ap.add_argument("--list", action="store_true",
                    help="just list the structs found, emit nothing")
    args = ap.parse_args(argv)

    records, typedef_map = parse_files(args.files)
    args.typedefs = typedef_map   # emit_type needs these to resolve aliases
    if not records:
        sys.exit("error: no structs with named fields found.")

    # dedupe by natvis name, keep first
    seen, uniq = set(), []
    for r in records:
        if r.natvis_name and r.natvis_name not in seen:
            seen.add(r.natvis_name)
            uniq.append(r)

    if args.list:
        for r in uniq:
            names = "/".join(r.all_names)
            print(f"{r.kind:6} {names:24} {len(r.fields)} fields  "
                  f"({os.path.basename(r.src_file)}:{r.line})")
            for n, t in r.fields:
                print(f"           {t:28} {n}")
        return 0

    wanted = uniq
    if args.struct:
        want = set(args.struct)
        wanted = [r for r in uniq if want & set(r.all_names)]
        missing = want - {n for r in wanted for n in r.all_names}
        if missing:
            sys.exit(f"error: struct(s) not found: {', '.join(sorted(missing))}\n"
                     f"  available: {', '.join(r.natvis_name for r in uniq)}")

        # Pull in structs referenced by the wanted ones ("typenames in struct").
        by_name = {n: r for r in uniq for n in r.all_names}
        queue, chosen = list(wanted), {r.natvis_name: r for r in wanted}
        while queue:
            r = queue.pop()
            for _, t in r.fields:
                dep = resolve(base_type(t), typedef_map)
                d = by_name.get(dep)
                if d and d.natvis_name not in chosen:
                    chosen[d.natvis_name] = d
                    queue.append(d)
        wanted = list(chosen.values())

    body = "\n\n".join(emit_type(r, opts=args, allocator=args.allocator)
                        for r in wanted)
    xml = HEADER + body + "\n" + FOOTER
    # Final safety net: neutralise "--" inside any XML comment we emitted.
    xml = re.sub(r"<!--(.*?)-->",
                 lambda m: "<!--" + xml_comment_safe(m.group(1)) + "-->",
                 xml, flags=re.S)

    # Validate. If we emit malformed XML, natvis dies silently and the user
    # spends an afternoon on it -- so fail loudly here instead.
    try:
        ET.fromstring(xml)
    except ET.ParseError as e:
        sys.exit(f"internal error: generated invalid XML ({e}). "
                 f"Please report the struct that triggered it.")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(xml)

    print(f"wrote {args.out}  ({len(wanted)} type(s): "
          f"{', '.join(r.natvis_name for r in wanted)})")
    if args.allocator and not (args.size_expr and args.used_expr):
        print("\nNOTE: Size() and/or IsUsed() were GUESSED from field names.")
        print("Verify the alignment mask, the flag polarity, and whether your")
        print("size includes the header. Sanity check: allocate a block of a")
        print("known size and confirm Size() reports that number.")
        print("Override with --size-expr / --used-expr.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
