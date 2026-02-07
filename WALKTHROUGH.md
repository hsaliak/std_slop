# Walkthrough

1.  **Clone the project:**
    ```bash
    git clone https://github.com/hsaliak/std_slop.git 
    cd std_slop
    ```
2.  **Ensure [Bazel](https://bazel.build/) is installed.**

    ```bash
    bazel test ...
    bazel build ...
    ```

Copy the `std_slop` executable to your path (e.g., `~/bin/`). Ensure `~/bin` is in your shell's `PATH` environment variable.

```bash
cp bazel-bin/std_slop $HOME/bin/std_slop
```

Create and save an OAuth token with Gemini (it comes with generous limits):

```bash
./slop_auth.sh
```

Create an example folder:

```bash
mkdir -p ~/Source/christmas-tree
```

Run `std_slop`. I recommend starting with Gemini and using your own OAuth. You can also get a key from Google AI Studio or use a model from OpenRouter. I recommend `gemini-3-flash-preview`. It's a cost-effective model that gets things done.

```bash
std_slop --google_oauth
/model gemini-3-flash-preview
```

This model defaults to `gemini-2.5-flash`. It does not support the `/models` command, but `/stats` should show usage stats in a second table. You may need to run at least one query with the LLM before doing so.

`std_slop` will have created a new database.

### Adding Skills

Let's add some skills.

**Prompt:**
```text
Add a skill to the database for an expert_rust_developer who follows best practices, minimizes dependencies, and keeps things simple.
```

Now let's add a `Planner` skill to help elaborate and break down vague requests into precise step-by-step plans.

**Prompt:**
```text
Add a Planner skill who helps elaborate and break down a vague request to a precise step-by-step plan. They do not write or modify files; they help the user iterate on their plans and ask clarification questions when needed.
```

Edit the planner skill to make any desired changes:

```bash
/skill edit planner
```

Now let's activate the planner:

```bash
/skill activate planner
```

### Sessions and Context

Let's get curious about the weather in Addis Ababa. Switch to a chat session. You might notice that skills are cross-cutting; this is by design.

```bash
/session switch chat
```

**Prompt:**
```text
What is the weather in Addis Ababa?
```

Sessions maintain their own context and are completely isolated. They can also be cleared and used for different purposes. You can also clone an existing session, essentially "forking it". 
They are free. Once you learn the weather, switch back to the default session:

```bash
/session switch default_session
```

If you want to branch off your current work to experiment without losing your progress, you can use:
```bash
/session clone experiment-1
```
This creates an identical copy of your current history and scratchpad.

Run `/message list` to see your recent interaction history and monitor token usage. You can also view the entire context. `/message` also lets you remove the last message and rebuild the context if needed.

```bash
/bash
/message list
/context show
```

### Building a Program

Let's continue building in Rust.

**Prompt:**
```text
Create a plan to write a program that displays a Christmas tree in ASCII art in Rust.
```

You will now see a plan. If the plan looks okay, deactivate the planner and activate the Rust expert.

```bash
/skill deactivate planner
/skill activate expert_rust_developer
```

And tell the LLM:

```text
Ok, implement it.
```

```text
          ★
          o
         ***
        *o@**
       +****o@
      **o@+****
     @+****o@+**
    ***o@+****o@+
   o@+****o@+****o
  ****o@+****o@+***
 *o@+****o@+****o@+*
         ###
         ###
```

**Note:** You don't need to activate the `expert_rust_developer` skill as the system prompt should be good enough to implement this program, but it feels good to be cool. This is especially useful to ensure that the generated code adheres to any specific coding standards relevant to the project.
**Note 2:** You can have multiple skills active (e.g., `planner` AND `expert_rust_developer`), but instructions might conflict as they all go into the system prompt.
**Note 3:** If you get rate limit errors, the `/throttle` command allows you to add wait times between requests. This is useful for free tier plans.

### Planning with the Scratchpad

As you work on complex features, you can use the **Scratchpad** to maintain an evolving plan.

1.  **Create a plan**: Ask the LLM to create a plan for a feature.
2.  **Save to Scratchpad**: Tell the LLM "Save this plan to the scratchpad so we can track our progress."
3.  **Autonomous Updates**: The LLM will use the `manage_scratchpad` tool to update the plan as you go, checking off items or adding new details.
4.  **Manual View**: You can see what's currently in the scratchpad at any time with:
    ```bash
    /session scratchpad read
    ```

### Reviewing Your Work
Before committing your changes, it's good practice to review them.

**Using the `code_reviewer` skill:**
1.  **Activate**: `/skill activate code_reviewer`
2.  **Request Review**: "Please review my current changes against Google C++ style."
3.  **Approve**: If the suggestions look good, tell the agent: "Apply these changes."

**Manual Review & Feedback:**
-   **`/review`**: Open the current git diff in an editor. Add lines starting with `R: ` to give instructions on specific code changes.
-   **`/feedback`**: Open the last assistant message in an editor with line numbers. Add lines starting with `R: ` to provide feedback on the assistant's response or reasoning.

In both cases, saving and exiting the editor will send your `R:` comments back to the LLM for immediate processing.

### Troubleshooting with Logs
If you encounter issues (e.g., API errors, tool failures), you can enable verbose logging or sink logs to a file:

```bash
# Sink logs to a file for later inspection
bazel run //:std_slop -- --log=debug.log

# See all internal events and request statuses in stderr
bazel run //:std_slop -- --stderrthreshold=0

# See full request and response bodies
bazel run //:std_slop -- --v=2 --stderrthreshold=0
```

### Pro-Tip: Batch Mode
For quick, one-off tasks, you don't even need to enter the interactive loop. Use the `--prompt` flag to run a single command and exit:

```bash
bazel run //:std_slop -- --prompt "Review the changes in main.cpp and suggest improvements"
```

### Advanced: Patch-Based Development (Mail Mode)
For non-trivial features, it's recommended to use the **Mail Model**.

1. **Enter Mail Mode**: `/mode mail`.
2. **Develop**: Work on a feature. Use `git_commit_patch` for logical steps.
3. **Review**: Run `/review patch` to see the series. Add `R: ...` comments for revisions.
4. **Finalize**: When everything is ready, say "LGTM" or call `git_finalize_series`.

This workflow ensures bisect-safe history and high-quality rationale for every change. See [docs/mail_model.md](docs/mail_model.md) for a full example.

### Finishing Up
Once you are done, you can exit the session with `/exit` or `/quit`. Happy slopping!
