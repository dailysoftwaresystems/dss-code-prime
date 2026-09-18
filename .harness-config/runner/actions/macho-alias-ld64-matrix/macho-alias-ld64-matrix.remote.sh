#!/bin/bash
# The MAC-SIDE half of macho-alias-ld64-matrix.sh. Not invoked directly: the
# driver base64s this file over the carriage and runs it there. See the
# driver's header for what is being measured and why.
set -u
CC=/usr/bin/clang
NM=/usr/bin/nm
OTOOL=/usr/bin/otool
W=/tmp/dss-macho-alias-matrix
rm -rf "$W"; mkdir -p "$W"; cd "$W" || exit 90

# ── The three producer variants, all with ONE body and TWO defined names at
#    the SAME address. They differ only in how the second name is declared.
#
#  plain     : a second label + `.globl`. This is the shape a DSS writer would
#              emit if it simply appended one more nlist at the same n_value
#              with n_desc = 0.
#  altentry  : the same, plus `.alt_entry`, i.e. n_desc = N_ALT_ENTRY (0x0200)
#              -- the shape DSS already stamps on its synthetic block labels.
#  setalias  : `.globl` + `.set`, the ATTRIBUTION CONTROL. A clang-authored
#              alias. If a cell fails here too, the behaviour is ld64's and not
#              a DSS emission bug.
cat > body.h.s <<'ASM'
.section __TEXT,__text,regular,pure_instructions
.globl _canon
.p2align 2
_canon:
    mov w0, #42
    ret
ASM

{ cat body.h.s; printf '.globl _alias\n.set _alias, _canon\n'; } > setalias.s
{ printf '.section __TEXT,__text,regular,pure_instructions\n.globl _canon\n.globl _alias\n.p2align 2\n_canon:\n_alias:\n    mov w0, #42\n    ret\n'; } > plain.s
{ printf '.section __TEXT,__text,regular,pure_instructions\n.globl _canon\n.globl _alias\n.alt_entry _alias\n.p2align 2\n_canon:\n_alias:\n    mov w0, #42\n    ret\n'; } > altentry.s

# The caller references ONLY the alias -- the whole point. If ld64 mints a
# zero-length atom for the alias, a reference that reaches only the alias can
# keep that atom while `-dead_strip` removes the body: perfect bytes, wrong
# program. `rc=0` cannot see that; the EXIT CODE can.
printf 'int alias(void);\nint main(void){ return alias(); }\n' > callalias.c
printf 'int canon(void);\nint main(void){ return canon(); }\n' > callcanon.c

fail=0
row() { printf '%-10s %-12s %-8s %-8s %-9s %-9s %s\n' "$@"; }
row VARIANT REFERENCED DEADSTRIP LINK EXITCODE SAMEADDR TEXTSIZE

probe() {   # probe <variant> <asmfile> <caller.c> <referenced> <deadstrip 0|1>
    v=$1; asm=$2; caller=$3; ref=$4; ds=$5
    tag="$v-$ref-ds$ds"
    "$CC" -c -o "$tag.o" "$asm" 2>"$tag.asm.err" || { row "$v" "$ref" "$ds" ASMFAIL - - -; fail=1; return; }
    ldflags=""; [ "$ds" = "1" ] && ldflags="-Wl,-dead_strip"
    # shellcheck disable=SC2086
    if ! "$CC" $ldflags -o "$tag.exe" "$caller" "$tag.o" 2>"$tag.link.err"; then
        row "$v" "$ref" "$ds" LINKFAIL - - -
        fail=1; return
    fi
    ./"$tag.exe"; ec=$?
    ca=$("$NM" -n "$tag.exe" | awk '$3=="_canon"{print $1}')
    aa=$("$NM" -n "$tag.exe" | awk '$3=="_alias"{print $1}')
    same=no
    if [ -n "$ca" ] && [ "$ca" = "$aa" ]; then same=yes
    elif [ -z "$ca" ] || [ -z "$aa" ]; then same="missing($ca/$aa)"; fi
    ts=$("$OTOOL" -l "$tag.exe" | awk '/sectname __text/{f=1} f&&/size /{print $2; exit}')
    row "$v" "$ref" "$ds" ok "$ec" "$same" "$ts"
    # ★ rc=0 IS NOT AN OBSERVABLE. The program must RUN and return 42, and both
    #   names must resolve to ONE address. A stripped body behind a zero-length
    #   atom shows up here and nowhere else.
    [ "$ec" = "42" ] || { echo "  ^^ EXIT CODE NOT 42"; fail=1; }
}

for v in plain altentry setalias; do
    for ds in 0 1; do
        probe "$v" "$v.s" callalias.c alias "$ds"
    done
done
# Canonical-only control: the alias is never referenced. If -dead_strip is
# going to remove a body it should not, this is the arm that says whether the
# alias had anything to do with it.
for ds in 0 1; do
    probe plain plain.s callcanon.c canon "$ds"
done

echo
echo "n_desc of _alias in each producer variant (0x0200 = N_ALT_ENTRY):"
for v in plain altentry setalias; do
    "$CC" -c -o "nd-$v.o" "$v.s" 2>/dev/null
    printf '  %-9s %s\n' "$v" "$("$NM" -m "nd-$v.o" | grep -E '_alias|_canon' | tr '\n' ' ')"
done
echo
echo "MATRIX_FAIL=$fail"
exit "$fail"
