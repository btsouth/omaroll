# Search and similar pictures

The search box matches filenames, captions and the text inside pictures. Text
is read locally by Tesseract and kept in a private cache. By default it is
read while a search is open, one picture at a time, screenshots first; the
status line counts the pictures read, and a notice under the results says
when some have not been read yet.

**Index text in the background** in Settings reads the library once its scan
has settled instead, two to four pictures at a time at the lowest CPU and disk
priority, and pauses while the computer runs on battery. Its status reads
"Text indexed N of M". **Screenshots only** limits it to screenshots; a search
still reads other pictures as before.

**Similar pictures** in Browse groups copies of a picture that were resized,
recompressed or brightened, and close bursts. Each set is a section with its
size and a Compare button. A candidate pair must match on a fine thumbnail,
not just a small hash, and every picture in a set must match every other, so
sets do not chain. Screenshots of text need a near-identical match, and when
both pictures have recognized text, the text must agree. **Match** sets how
close: Strict keeps only near-identical copies, Loose also finds recrops and
heavier edits.

Compare shows each picture's dimensions, file size, date and folder, and marks
the largest. **Keep this** moves the others to Trash after a confirmation, as
one step that Undo puts back. Exact and similar matches are only suggestions:
nothing is removed without that confirmation.

Duplicate and similarity hashes are kept in `~/.cache/omaroll`, keyed by path,
size and modification time, so a relaunch does not read every file again.
