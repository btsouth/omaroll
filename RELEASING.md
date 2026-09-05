# Releasing Omaroll

Start with [current status](docs/STATUS.md). v1.4.0 is the latest release in the
5 September 2026 handoff. Merged work and draft PR #6 do not constitute a new
release; no next version has been selected.

1. Update the version in `CMakeLists.txt`, AppStream metadata, the changelog,
   README package command, and public feature descriptions.
2. Run a clean Release build, tests through `tests/run-isolated.sh`, metadata
   validation, and a staged install. See [validation](tests/README.md).
3. Run the sanitizer build and render every deterministic view inside the local
   audio sandbox. Check rendered video with OpenGL.
4. Test the exact candidate on a real Omarchy desktop using the checklist in README.
5. Tag the approved commit as `vX.Y.Z` and push the tag.
6. Confirm the Release workflow tests the package lifecycle and publishes the
   source archive, Arch package, checksums, and signed provenance.
7. Download the public artifacts and verify their checksums before announcing.

Record the exact candidate commit and any untested hardware or desktop behavior.
Upstream package acceptance and MIME defaults remain separate from releasing.

Do not publish while a source checksum or package URL is a placeholder.
