# Release checklist

1. Build the native program and run the UI/MCP checks.
2. Run scripts/redact_scan.py with --public against the staged tree.
3. Inspect the staged file list and reject generated binaries and private paths.
4. Confirm the repository is public only after the scan is clean.

