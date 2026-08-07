# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

## Build & Test

_Add your build and test commands here_

```bash
# Example:
# npm install
# npm test
```

## Architecture Overview

_Add a brief overview of your project architecture_

## Conventions & Patterns

_Add your project-specific conventions here_

<!-- BEGIN KATA (managed by `kata init --with-agents`) -->
## kata issue tracker

This project uses [kata](https://github.com/kenn-io/kata) as its shared issue
ledger. Run `kata quickstart` at the start of each session for the full agent
contract. The short version:

- Search before creating: `kata search "<keywords>" --agent`.
- Prefer updating existing issues over duplicates (`kata comment`, `kata label add`, `kata edit`).
- Default to `--agent` for ordinary reads and mutations; use `--json` only when a script needs structured data.
- Close only verified work: `kata close <ref> --done --message "<scope + verification>" --commit <sha>`.
- If work is incomplete, label `needs-review` and comment what remains rather than closing.
- Never `kata delete` or `kata purge` without explicit user authorization.

## kata work.* conventions (agent orchestration)

When working a kata-tracked issue, keep its `work.*` metadata truthful
(see docs/operations/agent-orchestration.md for the full recipe):

- On claim/start: `kata meta set <ref> work.attention ok`; if the work has a
  dedicated branch, stamp it once with `kata meta set <ref> work.branch <branch>`.
- Signal live state: `kata meta set <ref> work.attention stuck|needs-human|ok`
  plus a one-line `work.attention_msg` saying why. Raise `stuck` when you cannot
  proceed, `needs-human` when you want review; clear back to `ok` when unblocked.
- Never stop with the signal stale: close the issue, or leave the attention
  pair reflecting the hand-off.
- Coordinators read `work.*` on issues they delegated; only the working agent
  writes them. `work.*` on closed issues is meaningless.
<!-- END KATA -->
