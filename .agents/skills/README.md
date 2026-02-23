# Skills in std::slop

Skills are modular sets of instructions and tools that can be activated on-demand.

## Structure
Each skill lives in its own directory under `.agents/skills/`:
```
.agents/skills/
  my-skill/
    SKILL.md
```

## SKILL.md Format
The `SKILL.md` file must start with YAML frontmatter:

```markdown
---
name: my-skill
description: "A description of what this skill does"
---

# Instructions
Detailed instructions for the agent when this skill is active.
You can use standard Markdown here.
```

## Usage
- Put your skill in `.agents/skills/my-skill/SKILL.md`.
- Run `/skill reload` in the agent.
- Run `/skill activate my-skill` to enable it.
