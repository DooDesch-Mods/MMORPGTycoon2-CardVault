# Release automation

The release version is shared by CMakeLists.txt, mod.json and the native plugin query. Add one matching CHANGELOG.md section before tagging vX.Y.Z on main.

The Windows build runs CTest, validates the mod against the pinned framework runtime, packages only the DLL, manifest, player guide, changelog and license notices, and retains its SHA-256. A tag publishes the verified ZIP and checksum file on GitHub.

The Nexus workflow uploads that exact GitHub ZIP, adds the same version's changelog and sends a Discord embed with the same grouped notes, a small thumbnail and download links. Delivery receipts are stored on the GitHub release. An interrupted Discord send blocks replay until its existing message is identified.

Required organization secrets: NEXUSMODS_API_KEY and DISCORD_RELEASE_WEBHOOK. Required repository variables: NEXUS_CARDVAULT_FILE_ID and NEXUS_CARDVAULT_MOD_ID. integrations/nexus/targets.json identifies the matching Nexus page and existing file group. A new Nexus listing and its first file must be created before setting these IDs. Until then, the release build publishes to GitHub and skips the Nexus job.

Use the Publish Nexus files workflow to validate or finish delivery for an existing GitHub release. Leave publish and announce false for a read-only validation. Enable them only for an authorized release. Inspect retained destination receipts before retrying failed writes.
