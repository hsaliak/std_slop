# AI Context Injection: AGENTS.md and SKILLS.md

This project uses a file-based context injection system to define AI personas and modular capabilities.

## AGENTS.md (Global Context)
This follows the specification at [https://agents.md](https://agents.md).
The `AGENTS.md` file in the root directory defines the global instructions and persona for the agent.
- **Injected Verbatim**: The entire content is added to every prompt turn.
- **Singleton**: Only one `AGENTS.md` is active at a time.
- **Usage**: Define project rules, coding standards, and long-term goals.

## SKILLS.md (Modular Capabilities)
This follows the specification at [https://agentskills.io/](https://agentskills.io/).
Skills are located in the `.agents/skills/` directory. Each skill is defined by a directory containing a `SKILL.md` file.
- **On-Demand**: Skills must be explicitly activated using `/skill activate <name>`. The `hey` hotword can be used to activate a skill for a single turn.
- **Automatic**: Skills can also be activated by the LLM using `tools.use_skill` in the Lua Control Plane.
- **Structured**: Uses YAML frontmatter for metadata (name, description).
- **Extensible**: Allows adding specialized knowledge (e.g., C++ expert, DBA) without bloating the global context.

## Differences
| Feature | AGENTS.md | SKILLS.md |
|---------|-----------|-----------|
| **Scope** | Global (Root) | Modular (`.agents/skills/`) |
| **Activation** | Automatic (if loaded) | Explicit (`/skill activate`) |
| **Format** | Verbatim Markdown | Markdown with Frontmatter |
| **Purpose** | Core Persona | Specialized Expertise |
