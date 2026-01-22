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
/session activate chat
```

**Prompt:**
```text
What is the weather in Addis Ababa?
```

Sessions maintain their own context and are completely isolated. They can also be cleared and used for different purposes. They are free. Once you learn the weather, switch back to the default session:

```bash
/session activate default_session
```

Run `/message list` to ensure that you have not polluted your context with details about weather. You can also view the entire context. `/message` also lets you remove the last message and rebuild the context if needed.

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

**Note:** You don't need to activate the `expert_rust_developer` skill as the system prompt should be good enough to implement htis program, but it feels good to be cool. This is especially useful to ensure that the generated code adheres to any specific coding standards relevant to the project.
**Note 2:** You can have multiple skills active (e.g., `planner` AND `expert_rust_developer`), but instructions might conflict as they all go into the system prompt.
**Note 3:** If you get rate limit errors, the `/throttle` command allows you to add wait times between requests. This is useful for free tier plans.

### Sequential Workflows with Todos

`std::slop` includes a built-in task management system that can be used to automate complex workflows.

1.  **Add tasks to a group:**
    ```bash
    /todo add my_feature "design the interface"
    /todo add my_feature "implement core logic"
    /todo add my_feature "write unit tests"
    ```

2.  **View your tasks:**
    ```bash
    /todo list my_feature
    ```

3.  **Automate execution:**
    Activate the `todo_processor` skill. This skill instructs the agent to fetch the first 'Open' task from the specified group, plan its implementation, and ask for your approval to proceed.

    **Prompt:**
    ```text
    Do the next todo on my_feature
    ```

    After each task is completed, the agent will mark it as `Complete` and you can prompt it to proceed to the next one.

### Troubleshooting with Logs

If you encounter issues (e.g., API errors, tool failures), you can enable verbose logging:

```bash
# See all internal events and request statuses
bazel run //:std_slop -- --stderrthreshold=0

# See full request and response bodies
bazel run //:std_slop -- --v=2 --stderrthreshold=0
```
