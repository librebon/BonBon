# Bonbon `.bon` document (MIME) icon

Thin-border pink dog-eared document icon with the Bonbon **Bn** glyph. Installed
as `application-x-bonbon.png` in `../mimetypes/<size>/` and, for Bonbon packages,
into `hicolor/<size>/mimetypes`.

Libreworks lists `application/x-bonbon` in its desktop file so **Open With**
works, but it does not ship this MIME type or these icons — Bonbon remains
the default handler and icon owner.

## Regenerate

```bash
python3 generate_bonbon_doc_icon.py
```

Writes preview/QA images in this directory and copies the PNGs into
`../mimetypes/<size>/`. Requires Pillow and NumPy. `bn-glyph.png` is the
lettering source. Silhouette geometry is read from git commit `885d770` in
the repo root (four levels above this file).
