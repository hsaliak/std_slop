# Contributing

Submit feature requests and bug reports as GitHub issues. State the problem, expected behavior, and relevant constraints. An implementation plan is optional.

## Code Changes

Repository changes follow the mail workflow in [mail_mode.md](mail_mode.md). Keep patches focused and validate affected targets.

## Database Safety

Use parameterized `Database::Query` and `Database::Execute` calls for user-supplied values:

```cpp
db_->Query("SELECT * FROM messages WHERE session_id = ?;", {session_id});
```

Do not construct SQL with user-supplied string interpolation.

See [../AGENTS.md](../AGENTS.md) for repository coding and testing requirements.
