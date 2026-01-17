# std::slop

std::slop is a C++17 AI coding agent driven by a persistent SQLite ledger for session management and transparency.

## Features
- **Prompt-Ledger**: Optional Git integration to track and revert code changes per interaction. Displays `[pl]` in the prompt when active.
...

## Command Reference
- `/prompt-ledger [on|off]` Toggle git-based tracking of file changes per interaction.
- `/prompt-ledger <GID>` Display the git diff associated with a specific message group.
...
