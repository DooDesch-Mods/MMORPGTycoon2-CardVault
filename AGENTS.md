# Card Vault contribution guide

Read README.md, CHANGELOG.md and docs/releases.md. Documentation and development-facing text are English.

- Keep game internals in adapters/mt2. The framework provides the central launcher and native UI. Native C ABI 1 and Lua API 1.0.0 are distinct from framework and game versions.
- Build and run CTest for runtime changes. Run python tools/package.py for package changes. The release workflow validates the package with the pinned framework runtime validator.
- Keep original character PNG bytes. Imports add prefabs; they must not silently replace world characters or overwrite a different existing file.
- Network work runs in the worker; native game calls run on the game thread. Keep errors and unknown transfer results visible. Inspect destination state before repeating an unconfirmed write.
- Never include tokens, profiles, exported cards, original game files or private research in packages or logs.
- Changes to public release metadata do not establish new runtime acceptance. Real-game checks use a separate owned profile.
