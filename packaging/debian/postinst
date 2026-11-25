#!/bin/sh
# Upsert Libreworks/Bonbon default-app entries into /etc/xdg/mimeapps.list.
# Identical for both brand packages. Idempotent; never deletes other keys.
set -e
TARGET="${MIMEAPPS_LIST:-/etc/xdg/mimeapps.list}"
LW="application/x-libreworks=libreworks.desktop;io.github.librebon.BonBon.desktop;bonbon.desktop;"
BON="application/x-bonbon=io.github.librebon.BonBon.desktop;bonbon.desktop;libreworks.desktop;"
mkdir -p "$(dirname "$TARGET")"
tmp=$(mktemp)
if [ ! -f "$TARGET" ]; then
    printf '%s\n' "[Default Applications]" "$LW" "$BON" > "$tmp"
    mv "$tmp" "$TARGET"
    exit 0
fi
in_defaults=0
have_defaults=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        "[Default Applications]")
            in_defaults=1
            have_defaults=1
            printf '%s\n' "$line" >> "$tmp"
            ;;
        \[*)
            if [ "$in_defaults" -eq 1 ]; then
                printf '%s\n' "$LW" "$BON" >> "$tmp"
                in_defaults=0
            fi
            printf '%s\n' "$line" >> "$tmp"
            ;;
        application/x-libreworks=*|application/x-bonbon=*)
            # Only replace these keys in [Default Applications]; leave other sections alone.
            if [ "$in_defaults" -ne 1 ]; then
                printf '%s\n' "$line" >> "$tmp"
            fi
            ;;
        *)
            printf '%s\n' "$line" >> "$tmp"
            ;;
    esac
done < "$TARGET"
if [ "$in_defaults" -eq 1 ]; then
    printf '%s\n' "$LW" "$BON" >> "$tmp"
elif [ "$have_defaults" -eq 0 ]; then
    printf '%s\n' "[Default Applications]" "$LW" "$BON" >> "$tmp"
fi
mv "$tmp" "$TARGET"
exit 0
