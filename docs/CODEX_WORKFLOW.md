# Codex CLI development with GPT-6 Astra

The repository's [.codex/config.toml](../.codex/config.toml) selects
`gpt-6-astra` with `model_reasoning_effort = "max"`. This preserves the
development machine's existing reasoning level; it is not a recommendation
that every small task needs maximum effort. The [official migration guide](https://developers.openai.com/api/docs/guides/latest-model)
recommends preserving a compatible reasoning level. The exact requested target
is GPT-6 Astra, not a moving "latest model" alias.

This is a Codex CLI workflow change, not an OpenAI API application migration.
No firmware, BACnet dependency, provider, authentication, permission, service
tier, or CI setting needs to change with the model. The repository contains no
OpenAI SDK request path to migrate. No context-window override is added; Codex
uses the model's own metadata. Code review uses the session model unless the
user deliberately configures a separate review model.

## Start and resume

From the repository root:

```sh
codex --version
codex
```

Trust the project through Codex's normal trust flow after reviewing its files.
Project configuration is skipped for untrusted projects. Explicit CLI flags
override project defaults; authentication and machine settings stay in the
user's own configuration. See [configuration precedence](https://learn.chatgpt.com/docs/config-file/config-basic).

To explicitly select Astra for a new task or a resumed CLI session:

```sh
codex --model gpt-6-astra
codex --model gpt-6-astra resume <SESSION_ID>
```

For a routine task where lower reasoning effort is appropriate, override it
for that invocation without editing shared defaults:

```sh
codex --model gpt-6-astra -c 'model_reasoning_effort="medium"'
```

Astra supports `low`, `medium`, `high`, `xhigh`, and `max`; do not carry forward
`none` or `minimal`. See the [model reference](https://developers.openai.com/api/docs/models/gpt-6-astra).
If an older CLI rejects the model or effort, inspect `codex --version` and its
available update command. Do not silently substitute a different model or
weaken permissions. This workflow is validated against Codex CLI 0.153.4.

## Instructions and working boundaries

The root [AGENTS.md](../AGENTS.md) supplies project-specific follow-through,
parallel-work, verification, and hardware-evidence guidance. Personal guidance
remains in `~/.codex/AGENTS.md` (or the configured Codex home). Project model
selection is configuration, not an instruction to copy credentials or local
machine settings into the repository.

Start a fresh CLI session after changing instruction files; instruction
discovery occurs at session startup. A deeper `AGENTS.override.md` or
`AGENTS.md` may supply more specific guidance. Audit conflicting instructions
instead of adding repeated prompts or bypass flags. See [instruction discovery](https://learn.chatgpt.com/docs/agent-configuration/agents-md).

For a normal code task, implement and run the checks appropriate to the
affected component in [DEVELOPMENT.md](DEVELOPMENT.md). For field work, use the
current authorization and the appropriate hardware/soak workflow. A model
migration never implies permission to actuate relays, flash controllers, or
publish a release.

## Check the CLI setup

These local diagnostic commands validate configuration and expose effective
model metadata or instruction discovery without asking a model to change code:

```sh
codex doctor --summary
codex features list
codex debug models --bundled
codex debug prompt-input --disable hooks "Configuration inspection only"
git diff --check
```

Inspect model-catalog and prompt output locally: it can include machine paths,
personal instructions, and installed skill descriptions. Do not commit it as
project documentation or send it to an unrelated service. A bundled catalog
confirms CLI recognition, not account access or a successful server request.

An optional one-response smoke test uses the existing Codex login. It contacts
the model service and consumes normal account usage, but explicitly requests
no tools or device access:

```sh
codex --ask-for-approval never exec --strict-config --ephemeral \
  --sandbox read-only --disable hooks \
  "Do not call tools, read files, or access devices. Reply exactly ASTRA_READY."
```

The inspection and smoke commands disable lifecycle hooks for that invocation
only; they do not change saved hook settings. Review enabled integrations before
running a smoke test because integration startup is independent of the prompt.
Successful configuration checks or a smoke response are not firmware regression
or physical-device acceptance results.
