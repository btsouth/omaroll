# Releasing Omaroll

1. Update the version in `CMakeLists.txt`, AppStream metadata, the changelog,
   README package command, and public feature descriptions.
2. Run a clean Release build, the tests, metadata validation, and a staged install.
3. Run the sanitizer build and render every deterministic view.
4. Test the exact candidate on a real Omarchy desktop using the checklist in README.
5. Tag the approved commit as `vX.Y.Z` and push the tag.
6. Confirm the Release workflow tests the package lifecycle and publishes the
   source archive, Arch package, checksums, and signed provenance.
7. Download the public artifacts and verify their checksums before announcing.

Do not publish while a source checksum or package URL is a placeholder.
