# Omaroll privacy

Omaroll is local-first. It has no analytics, telemetry, accounts, advertising,
or network service.

It reads media from Omarchy's capture folders, your standard Pictures, Videos
and optional Downloads folders, and any other folders you add. Settings,
favourites, hidden items, album membership, and file identities are stored
under `$XDG_CONFIG_HOME/omaroll`.
Generated thumbnails are stored under `$XDG_CACHE_HOME/omaroll` and are bounded
by the cache limit in Settings. When Tesseract is installed, typing at least two
search characters starts a local, one-picture-at-a-time text index. Extracted
text stays in a private cache under the same directory and is replaced when the
source file changes. The cache is pruned to 64 MB and stale entries are removed
locally when Omaroll starts. Clear cache in Settings removes the extracted text
without touching any media.

After folder discovery, Omaroll asks the locally installed ImageMagick or
ffprobe process for the original date embedded in each general photo or video,
one file at a time. Found dates and the absence of a date are stored in a
private, identity-checked cache at
`$XDG_CACHE_HOME/omaroll/media-dates.index`, bounded at 16 MB. This changes only
library ordering. It does not write to, move, or copy media.

Exact-duplicate review runs only when you choose it under Browse. Omaroll
hashes same-size candidates locally on a worker thread and keeps those hashes
in memory for the current session. It sends nothing away and removes nothing.

Opening the viewer asks the locally installed ImageMagick or ffprobe process
for technical details about that one file. Those details are kept in memory
only and are replaced when you open another file.

Omaroll does not upload media. External actions hand a local path to the tool
named in the interface. Moving an item to Trash uses the desktop trash service.
Removing Omaroll does not remove its settings, thumbnail cache, text index, or
media date index.
