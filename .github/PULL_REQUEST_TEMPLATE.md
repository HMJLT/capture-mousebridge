## What changed

<!-- Describe the user-visible behavior and the protocol/API impact. -->

## Validation

- [ ] `python scripts/redact_scan.py . --public`
- [ ] `python -m unittest discover -s tests -p 'test_*.py'`
- [ ] `build.bat` (when native code changed)

## Compatibility

- [ ] UDP 16-byte packets remain backward compatible.
- [ ] Serial commands remain backward compatible.
- [ ] MCP tools and docs were updated together.
