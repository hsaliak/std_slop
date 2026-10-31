# Name: tmux
# Description: Safely inspect and use tmux sessions, windows, and panes; fall back cleanly when std_slop is not running in tmux.

Use this skill when the user asks to inspect or control a tmux session, open windows for tests, or check whether `std_slop` or `sl` is running inside tmux. Read the installed `man tmux` when a command detail is unclear.

## Detect a usable tmux pane

`tmux` sets `TMUX_PANE` in the environment of pane child processes. Use it as the primary check and verify that the pane is still reachable:

```sh
if [ -z "${TMUX_PANE:-}" ] || ! command -v tmux >/dev/null 2>&1; then
  echo "This process is not in a usable tmux pane."
  exit 0
fi
if ! pane_info=$(tmux display-message -p -t "$TMUX_PANE" \
  '#{session_name}:#{window_index}.#{pane_index} #{pane_current_command} #{pane_current_path}' 2>/dev/null); then
  echo "The tmux pane is unavailable."
  exit 0
fi
printf '%s\n' "$pane_info"
```

If `TMUX_PANE` is missing or the query fails, do not run commands against an implicit current session, attach to another session, or start a new tmux server. Say that tmux is unavailable for this process and fall back to equivalent non-tmux shell steps. A present but stale `TMUX` value is not proof that this pane can be reached.

If the user asks whether the agent is running inside `std_slop` or `sl`, inspect `pane_current_command` from the verified pane. If it is not `std_slop` or `sl`, report that the app was not detected in that pane. Do not send app input to a shell or another application.

## Inspect before acting

Tmux has persistent sessions; each session links windows; each window contains one or more pane terminals. A window can be linked in more than one session. Prefer stable pane IDs such as `$TMUX_PANE`, or a fully qualified `session:window` target, over ambiguous window numbers.

Useful read-only checks:

```sh
tmux display-message -p -t "$TMUX_PANE" \
  '#{session_name}:#{window_index}.#{pane_index} #{pane_current_command} #{pane_current_path}'
tmux list-sessions -F '#{session_name} windows=#{session_windows}'
tmux list-windows -a -F '#{session_name}:#{window_index} #{window_name} #{pane_current_path}'
tmux list-panes -a -F '#{session_name}:#{window_index}.#{pane_index} #{pane_current_command} #{pane_current_path}'
```

Use `pane_current_command` and `pane_current_path` to identify a target before sending input. Do not assume that a pane with a familiar name is still the same process.

## Open a task window and collect output

When the user asks for a separate test window, create it in the verified session, give it a descriptive name, and set its initial directory explicitly:

```sh
session=$(tmux display-message -p -t "$TMUX_PANE" '#{session_name}')
tmux new-window -d -t "$session:" -n smoke-test -c "$PWD"
```

`new-window -d` does not switch the current session to the new window. `-c` sets its starting directory. If a shell command is supplied to `new-window`, the window closes when that command exits unless `remain-on-exit` is configured; prefer opening a shell and then running the test in it when the user needs to inspect the results.

Only send keys after confirming the new pane and its path. `send-keys` sends literal characters and named keys; sending `Enter` executes the command. Quote shell arguments correctly, and do not send a command to the user's active `std_slop` input pane unless the user explicitly asks for that interaction.

```sh
tmux list-windows -a -F '#{session_name}:#{window_index} #{window_name} #{pane_current_path} #{pane_current_command}'
tmux send-keys -t "$session:smoke-test" 'pwd; printf "smoke test ready\\n"' Enter
```

Capture output without changing the pane:

```sh
tmux capture-pane -t "$session:smoke-test" -p -S -80
```

`-p` writes captured pane contents to stdout; `-S` selects the start line. Treat captured output as private: it may contain prompts, file contents, or secrets. Review only the needed lines and do not repeat secrets in the response.

## Safety rules

- Do not kill sessions, windows, or panes, detach clients, respawn panes, or send interrupt keys unless the user explicitly asks.
- Do not switch the user's active window or pane just to inspect a task; use detached windows (`-d`) when appropriate.
- Do not send credentials or tokens with `send-keys`; commands may remain in pane history or process listings. Use approved secret configuration.
- Do not close a test window automatically if the user may want to inspect it. Close only windows created for the task, and only when cleanup is requested or clearly authorized.
- Do not infer that tmux is a security boundary. Pane processes run with the same operating-system permissions as their user.
- If any check fails, stop tmux actions, report the exact failure, and continue with normal shell guidance where possible.
