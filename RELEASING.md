# Releasing Omaroll

Start with [current status](docs/STATUS.md). v1.6.0 is the latest published
release. A version is released only when its tag is pushed after desktop
acceptance.

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
