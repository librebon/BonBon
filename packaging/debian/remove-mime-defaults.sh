#!/bin/sh
# Drop our default-app keys only when the other brand is not remaining and the
# keys still point at libreworks.desktop / bonbon.desktop. Only the
# [Default Applications] section is touched; Added / Removed Associations stay.
set -e
TARGET="${MIMEAPPS_LIST:-/etc/xdg/mimeapps.list}"
[ -f "$TARGET" ] || exit 0
pkg="${DPKG_MAINTSCRIPT_PACKAGE:-}"
other_still=0
for other in libreworks bonbon; do
    if [ "$other" != "$pkg" ]; then
        if dpkg-query -W -f='${Status}' "$other" 2>/dev/null | grep -q 'install ok installed'; then
            other_still=1
        fi
    fi
done
[ "$other_still" -eq 0 ] || exit 0
lw_ok="application/x-libreworks=libreworks.desktop;bonbon.desktop;"
bon_ok="application/x-bonbon=bonbon.desktop;libreworks.desktop;"
tmp=$(mktemp)
in_defaults=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        "[Default Applications]")
            in_defaults=1
            printf '%s\n' "$line" >> "$tmp"
            ;;
        \[*)
            in_defaults=0
            printf '%s\n' "$line" >> "$tmp"
            ;;
        "$lw_ok"|"$bon_ok")
            # Only drop our Default Applications lines; leave other sections alone.
            if [ "$in_defaults" -ne 1 ]; then
                printf '%s\n' "$line" >> "$tmp"
            fi
            ;;
        *)
            printf '%s\n' "$line" >> "$tmp"
            ;;
    esac
done < "$TARGET"
mv "$tmp" "$TARGET"
exit 0
