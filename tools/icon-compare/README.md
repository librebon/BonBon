# Panel icon comparison

`panel_icons.cpp` grabs every icon QToolButton of the Layers, Frames and Styles panels (one row
per panel) into a PNG. Compile it against any tree's `lwui`/`lwcore` to compare releases
pixel for pixel:

    ./compare.sh <commit-a> <commit-b> out-dir      # e.g. 36b9b8f HEAD /tmp/icons

`ENABLED=1` grabs the buttons enabled (default: disabled); building with `-DDARK` selects the
Dark theme. `docs/panel-icons-compare.png` was made this way (original 36b9b8f, 0.2.3, 0.2.4).
