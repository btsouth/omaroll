# Search and similar pictures

The search box matches filenames, captions, tags, album names, folder names
below the library's folders, camera and lens, the text inside pictures and the text of PDFs. Case and
accents are ignored, so `cafe` finds `café-paris.jpg` and `CAFÉ` finds "cafe".
Every word must match. Fields narrow the search:

| Type | To find |
|---|---|
| `tag:travel` · `album:iceland` | Tagged, or in an album. A tag also matches its nested tags |
| `in:pictures/2024` · `in:~/Pictures` | In a folder, written from home or in full |
| `camera:fuji` · `lens:35mm` | Taken with a camera or lens |
| `type:video` (or `kind:`) | `screenshot`, `recording`, `photo`, `video`, `download`, `pdf`, `raw`, `animated` |
| `before:2024` · `after:2023-06` · `after:2024-06-01` | Earlier or later than a year, month or day |
| `"exact phrase"` · `tag:"road trip"` | Words side by side |
| `-draft` · `-tag:work` · `-"old copy"` | Leave those out |

A field value matches the start of a word, so `camera:fuji` finds
"FUJIFILM X-T4" and `tag:japan` finds "Travel/Japan". `before:` is earlier
than the whole period and `after:` later, so `after:2023 before:2025` is 2024.
Anything else with a colon is searched as plain text. Inside quotes, `\"` is a
quote and `\\` a backslash, so `album:"Road \"Trip\""` names that album. Typing a field offers
the library's tags, albums, folders, cameras, kinds and years; Up and Down pick
one and Enter or Tab takes it. **?** lists the syntax too.

A search that finds nothing offers **Did you mean** with each unknown word
replaced by the closest word the library knows, from names, folders, tags,
albums, cameras, captions and text already read, so `invoce` offers
`invoice`, and tries again as more text is read. Nothing from hidden files is
offered unless hidden files are shown. One letter may differ in a word of four or five letters, two in a
longer one.

Choices in **Browse** combine: a folder, an album, a tag, a date, a camera or
lens and a minimum rating narrow together. Each shows as a chip under the
filter bar with its own clear button. **Save current view** keeps the search,
tokens included, the kind and sort, and the folder, album, tag, date, camera,
lens and rating chips. Duplicate and similar reviews are not saved. Deleting an
album or tag deletes the saved views that open it. Omaroll 1.18 and older open
such a view with only its plain words and without its album.

Picture text is read locally by Tesseract and kept in a private cache. By
default it is read while a search is open, one picture at a time, screenshots
first; the status line counts the pictures read, and a notice under the results
says when some have not been read yet, with a link to the text settings that
`Tab` reaches and `Enter` opens. Until
the first match, the results say the text is still being read rather than that
nothing matches. A match in picture or PDF text shows on the tile with the
matching word in the middle of the line, and PDF tiles carry a PDF badge.

PDF text comes from `pdftotext`, the first 50 pages of each document, read one
document at a time while a search is open or in the background, and cached the
same way. A document that takes longer than 20 seconds is skipped until it
changes. A library of long PDFs keeps most of their text in that cache rather
than in memory, and searches it there.

**Index text in the background** in Settings reads the library once its scan
has settled instead, two to four pictures at a time at the lowest CPU and disk
priority, and pauses while the computer runs on battery. PDFs are read in the
background too, one at a time, with the same pause. Its status reads
"Text indexed N of M". **Screenshots only** limits it to screenshots; a search
still reads other pictures as before.

**Similar pictures** in Browse groups copies of a picture that were resized,
recompressed or brightened, and close bursts. **Find similar pictures** in a
picture's menu, in the grid, the preview or the viewer, opens the review on
that picture's whole set, or says nothing similar was found. It sets the kind,
favourites, rating, folder, search and other choices aside while it shows the
set, and puts them back when you leave the review with its chip, `Esc` or the
mouse's Back button. Each set is a
section with its size and a Compare button, and each tile shows the end of
its path, folders and name, so copies can be told apart. **Exact duplicates** are sectioned the same way.
A set that another filter leaves with a single picture, such as a folder
holding one copy, is left out. A candidate pair must match on a fine
thumbnail, not just a small hash, and every picture in a set must match every
other, so sets do not chain. The one exception, at Normal, is a picture left
on its own that matches a picture in a set, such as the first or last frame
of a burst: it joins that set when it is within Loose of every picture in it,
and its tile says "Looser match". Strict and Loose have no looser members. Screenshots of text need a near-identical match, and when
both pictures have recognized text, the text must agree. **Match** sets how
close: Strict keeps only near-identical copies, Loose also finds recrops and
heavier edits.

Compare shows each picture's dimensions, file size, date and folder, and marks
the largest. **Keep this** moves the others to Trash after a confirmation, as
one step that Undo puts back. Exact and similar matches are only suggestions:
nothing is removed without that confirmation.

Duplicate and similarity hashes are kept in `~/.cache/omaroll`, keyed by path,
size and modification time, so a relaunch does not read every file again.
