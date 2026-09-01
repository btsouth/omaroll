# Omaroll privacy

Omaroll is local-first. It has no analytics, telemetry, accounts, advertising,
or network service.

It reads media from the folders you choose. Settings, favourites, hidden items,
album membership, and file identities are stored under `$XDG_CONFIG_HOME/omaroll`.
Generated thumbnails are stored under `$XDG_CACHE_HOME/omaroll` and are bounded
by the cache limit in Settings.

Omaroll does not upload media. External actions hand a local path to the tool
named in the interface. Moving an item to Trash uses the desktop trash service.
Removing Omaroll does not remove its settings or thumbnail cache.
