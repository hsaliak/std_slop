# `run_js`, RLMs, and Regular Coding Agents

This note compares the practical role of std::slop's `run_js` tool with
Recursive Language Models (RLMs), as described in arXiv `2512.24601v3`, and
with regular coding agents that call atomic tools such as read, write, edit,
grep, and shell execution one step at a time.

The short version: RLMs scale model cognition over very large prompts;
`run_js` scales local agent operations over trusted tools; regular coding agents
provide explicit step-by-step tool use. These are complementary rather than
mutually exclusive.

## What the RLM paper is solving

The RLM paper frames a long prompt as an external environment around a base
language model with a finite context limit. Instead of asking the base model to
consume the whole prompt directly, the scaffold lets the model inspect portions
of the prompt, decompose the problem, recursively call itself on subproblems,
and aggregate the sub-results.

Practically, this is a long-context reasoning architecture. It is aimed at
problems where the limiting factor is not access to files or shell commands, but
semantic coverage of very large inputs: long document QA, corpus-scale
aggregation, recursive summarization, and tasks where important evidence may be
spread across more text than the base model can hold in one context window.

## What our `run_js` implementation is solving

std::slop's `run_js` is a programmable local control plane. The model sends a
bounded JavaScript snippet, and the runtime exposes synchronous helper calls such
as:

- `tools.read_file(args)`
- `tools.list_directory(args)`
- `tools.grep(args)`
- `tools.execute_bash(args)`
- `tools.edit_tool("input_key")`
- `tools.write_file("input_key")`
- `tools.dispatch(name, args)` for host tools that are marked run-js-callable
- `tools.llm_query(args)` for bounded delegated reasoning
- `tools.persist_function(args)` for reusable deterministic helpers

The important implementation property is that `run_js` is not another open-ended
agent loop. It is a single local execution step with explicit helper validation,
synchronous host calls, and a JSON-serializable return value. It is best used to
compress mechanical operations that would otherwise require many model/tool
round trips.

For example, a coding task often needs to list files, inspect a few snippets,
search for a symbol, run a focused test, trim the output, and return only the
useful evidence. With regular atomic tools, each of those operations may require
a separate model turn. With `run_js`, the model can express that deterministic
workflow in one bounded script and return a compact object.

## Operational constraints in our implementation

`run_js` intentionally has guardrails because it can batch many operations:

- JavaScript snippets should be bounded and return compact JSON.
- The runtime exposes helper methods synchronously; snippets should not use
  top-level `await`, async wrappers, or recursive `run_js` calls.
- Payload-heavy mutation helpers require indirection through `run_js.input`:
  `tools.edit_tool("source_edit")` and `tools.write_file("generated_doc")`
  receive named input keys rather than direct inline payloads.
- Direct helper calls validate arguments before side effects.
- `tools.dispatch(name, args)` is for run-js-callable host tools, not a bypass for
  payload-key mutation rules.
- Shell commands must set explicit timeouts and should be literal or built only
  from validated allowlisted values.
- Returned data should be summarized or sliced so large logs, grep results, and
  file contents do not flood the model context.

These constraints make `run_js` closer to a deterministic orchestration tool than
to a free-form autonomous sub-agent.

## Regular coding agents with atomic tools

A regular coding agent usually operates like this:

1. The model reasons about the next step.
2. It calls one tool, such as `read_file` or `grep`.
3. The tool result is returned to the model context.
4. The model reasons again and chooses another tool.

This model is simple, auditable, and safe. Every action is visible as a separate
step. It is often the right interface for small edits, uncertain investigations,
or side effects that should be reviewed one at a time.

The tradeoff is operational overhead. Large investigations can become verbose:
raw outputs accumulate in context, repeated inspection patterns consume many
turns, and simple loops require repeated model decisions even when the procedure
is deterministic.

## Practical advantages of our approach

The advantages of std::slop's `run_js` are operational rather than cognitive.
For long-context reasoning, the fair comparison is mostly against conventional
single-tool calling: `run_js` is a step up when it can search, filter, and
summarize local evidence before the model sees it, because that reduces context
pressure and tool-call overhead. It is still not the same kind of step as an RLM:
RLMs change how a model recursively decomposes and aggregates a huge semantic
input, while `run_js` makes known local procedures cheaper, more compact, and
easier to validate.

Precise advantages:

- **Fewer model/tool round trips for mechanical work.** A single snippet can run a
  bounded sequence such as list, search, read selected ranges, run one focused
  test, and return a compact summary. This is useful when the next steps are
  procedural rather than judgment-heavy.
- **Local output reduction before context ingestion.** Logs, grep output, file
  ranges, and command results can be sliced or summarized inside the snippet.
  This avoids spending model context on intermediate data that is only needed to
  decide whether a check passed.
- **Deterministic loops and conditionals.** Simple loops over files, allowlisted
  targets, or validation cases can run in JavaScript without asking the model to
  choose each next call. That is valuable for repetitive repo inspection and
  validation sweeps.
- **One place to enforce orchestration guardrails.** The implementation can make
  helper calls synchronous, validate helper arguments, require explicit shell
  timeouts, and force large edit/write payloads through named `input` keys. These
  rules are easier to apply consistently than if every multi-step sequence is
  improvised across many turns.
- **Clear separation between reasoning and deterministic execution.** The outer
  model decides the plan; the snippet performs the mechanical part. When used
  well, this reduces accidental reasoning inside shell pipelines or ad hoc text
  processing.
- **Lower integration cost for coding-agent tasks.** `run_js` works over the same
  file, shell, edit, and host-tool primitives the agent already uses. It does not
  require training a model, designing a recursive summarization policy, or
  changing the repository tools into an RLM-specific environment.
- **Composable with specialized tools.** A snippet can combine ordinary helpers
  with domain tools exposed through `dispatch`, while still returning a small
  JSON result. This is practical for workflows such as "inspect, edit, run test,
  report evidence".
- **Reusable deterministic glue.** When the same orchestration pattern recurs,
  small helpers can be persisted instead of reimplemented in prompts. That is a
  narrower and more auditable form of reuse than relying on the model to remember
  a multi-step procedure.

These are real advantages for day-to-day coding-agent work. They do not imply
that `run_js` is universally superior.

Cases where `run_js` does not deliver the RLM benefit:

- **Tasks that require semantic decomposition across huge text.** RLMs are a
  better fit when the hard part is recursively understanding and aggregating a
  corpus larger than the context window. `run_js` can retrieve and summarize
  snippets, but it does not provide recursive model cognition by itself. Examples
  include answering a question that depends on comparing hundreds of long design
  documents, or synthesizing a consistent timeline from a very large chat archive
  where the relevant evidence is not known in advance.
- **Tasks where each step needs fresh judgment.** If every tool result changes the
  plan, atomic tool use is more transparent and often safer.
- **High-risk side effects.** Batched scripts can obscure the exact sequence of
  operations. Destructive actions, broad edits, or unclear shell commands should
  stay explicit and individually reviewed.
- **Simple one-step operations.** Calling `run_js` to wrap a single read or edit
  adds ceremony without benefit.
- **Untrusted or poorly bounded procedures.** The approach depends on small,
  validated snippets. If the script is large, open-ended, or returns huge raw
  output, it loses the main benefits and becomes harder to audit than atomic tool
  use.

So the forthright claim is: `run_js` is advantageous for bounded, mechanical,
local orchestration in coding-agent workflows. It is not an RLM substitute, and
it should not be used to hide complex judgment or risky side effects inside a
single script.

## Practical differences

### Reasoning layer versus operations layer

RLMs primarily change the reasoning layer. They give the model a recursive
strategy for reading and aggregating information from a prompt-like environment
that is too large to fit in context.

`run_js` primarily changes the operations layer. It lets the model compactly
execute deterministic local workflows over existing tools and return summarized
results.

Regular coding agents keep both layers explicit: the model reasons after each
single tool result, then chooses the next action.

### Context pressure

RLMs reduce context pressure by externalizing the prompt and recursively reading
subsets of it.

`run_js` reduces context pressure by filtering and summarizing tool output before
it reaches the model.

Regular atomic tool use tends to push more intermediate output directly into the
conversation unless the agent is very disciplined about ranges and limits.

### Control and auditability

Regular atomic tools are the most transparent: each operation is a separate
visible event.

`run_js` trades some of that step-by-step visibility for efficiency. A single
script can contain several reads, searches, checks, or edits, so snippets must be
kept small, bounded, and validation-heavy.

RLMs can be harder to audit because useful work may happen inside recursive
model calls and intermediate reasoning summaries.

### Where each fits in std::slop

Use regular atomic tools when the next step needs fresh model judgment or the
side effect should be individually visible.

Use `run_js` when the procedure is already clear and several local operations can
be safely batched, summarized, and validated in one step.

Use RLM-like recursion when the hard part is semantic decomposition over a very
large body of text or state. `run_js` can support such a design as the local
execution substrate, but it does not by itself implement RLM-style recursive
model cognition.

## Summary table

| Dimension | RLMs from arXiv 2512.24601v3 | std::slop `run_js` | Regular coding agent with atomic tools |
| --- | --- | --- | --- |
| Primary goal | Scale reasoning over very large prompts | Scale local tool orchestration | Provide explicit step-by-step environment access |
| Core abstraction | Recursive model calls over an external prompt environment | Bounded JavaScript control plane over host tools | One model-selected tool call at a time |
| Main bottleneck addressed | Context limit and long-horizon semantic aggregation | Tool-call round trips, repetitive local workflows, output bloat | Safe direct access to files, shell, and other primitives |
| Where intelligence lives | In the base model and recursive decomposition policy | Mostly in the outer model; JS is deterministic glue | In the model between every tool result |
| Uses recursive LLM calls by default | Yes | No | No |
| Can call delegated LLMs | Intrinsic to the architecture | Only through explicit helpers such as `tools.llm_query(args)` | Only if exposed as a separate tool |
| Best input shape | Huge text/prompt/corpus requiring recursive reading | Repo state, logs, command output, structured local tasks | Small-to-medium task state inspected incrementally |
| Best output shape | Aggregated answer from subproblem summaries | Compact JSON summary of local operations | Raw or lightly scoped tool result per turn |
| Strength | Long-context semantic scalability | Efficient deterministic orchestration with local summarization | Transparency, simplicity, and fine-grained control |
| Weakness | More complex to implement and audit; recursive errors can compound | A script can hide too much if not bounded and validated | Slow for mechanical multi-step workflows; can bloat context |
| Safety model | Depends on recursion policy and environment permissions | Helper validation, input-key mutation rules, bounded snippets | Tool-level validation per call |
| Auditability | Medium to low unless subcall traces are preserved | Medium; inspect the script and returned summary | High; each operation is visible |
| Fit for code editing | Indirect; useful for huge-codebase reasoning | Strong for inspection, validation, and carefully scoped edits | Strong for direct cautious edits |
| Fit for long document QA | Strong by design | Useful for retrieval/summarization, not a full RLM | Possible but turn-heavy |
| Practical analogy | Recursive research team reading a giant archive | A short local operations script with guarded tool access | Assistant manually using one tool at a time |

## Design implication

These mechanisms compose well. A mature std::slop workflow can use atomic tools
as safe primitives, `run_js` as a compact deterministic orchestration layer, and
RLM-style recursive model calls for tasks where the model must reason over far
more semantic material than a single context window can hold.
