# Context and Skills

`AGENTS.md` provides repository-specific instructions that are included in model requests when loaded.

Skills are modular instruction sets stored in `.agents/skills/<name>/SKILL.md`. They can be activated with `/skill activate <name>`, used for one turn with `hey <name> <query>`, or activated by the model through `tools.use_skill`.

See [.agents/skills/README.md](../.agents/skills/README.md) for the skill file format and authoring instructions.
