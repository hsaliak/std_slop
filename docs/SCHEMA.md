# std::slop Database Schema

`core/database.cpp` is the canonical schema definition. The database persists conversation state, tool and skill definitions, usage, scratchpads, project context, and mail-workflow metadata.

## Tables

| Table | Purpose |
| --- | --- |
| `messages` | User, assistant, and tool messages, grouped by interaction. |
| `tools` | Registered tool definitions and enablement state. |
| `skills` | Skill metadata and prompt patches. |
| `sessions` | Accordion context settings and active skills for each session. |
| `usage` | Provider-reported token usage. |
| `scratchpads` | Session-local scratchpad content. |
| `agent_md` | Loaded project-context files. |
| `patch_approvals` | Approved staging-branch heads. |
| `staging_branches` | Staging branch and parent-branch metadata. |
| `settings` | Singleton runtime settings, including mail-mode state. |

## Notes

- `messages` is keyed by `session_id` and records `role`, `content`, tool-call metadata, status, group ID, parsing strategy, and token count.
- `sessions` stores accordion retention and watermark settings. See [CONTEXT_MANAGEMENT.md](CONTEXT_MANAGEMENT.md).
- `scratchpads` reference `sessions` and are deleted with their session.
- The database initializer performs compatible additive migrations for existing databases; inspect `core/database.cpp` for exact columns, defaults, indexes, and migration behavior.
