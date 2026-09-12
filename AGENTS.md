# Project instructions

## Scope and follow-through

This repository contains ESP32-S3 BACnet/IP firmware, a browser installer,
management clients, and their tests. Codex CLI setup is documented in
[docs/CODEX_WORKFLOW.md](docs/CODEX_WORKFLOW.md).

- For implementation and fix requests, carry the authorized change through
  implementation and relevant verification. A proposed plan is not completion.
- Continue with routine, reversible local work and relevant read-only checks.
  Ask only when missing information materially affects correctness, scope, or
  authorization; finish unaffected work while awaiting an answer.
- Preserve the user's changes and unrelated work. Inspect the diff before
  editing; use narrow patches, and do not discard existing changes.
- Follow current user instructions over conflicting skill guidance, subject to
  higher-priority instructions and actual permissions. Identify the exact skill
  rule if it causes a pause or a material change of direction.
- Delegate independent, bounded research or review when it improves speed or
  quality. Give agents distinct ownership; keep live-device control with one
  coordinator and prohibit uncoordinated network tests or relay writes.

## Development and verification

- Use [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the pinned ESP-IDF 5.5.4,
  `esp32s3` target, bacnet-stack submodule, and release workflow. Do not upgrade
  these dependencies merely because the development model changed.
- For firmware, protocol, or management-client changes, run the relevant host
  tests with a modern Python; the full command is `tests/run_host_tests.sh`
  (Python 3.12+). Build firmware changes with `idf.py build` in the IDF
  environment. The IDF Python environment can differ from the host-test Python.
- For browser installer changes, follow the staging and test prerequisites in
  [docs/BROWSER_INSTALLER.md](docs/BROWSER_INSTALLER.md#local-development-and-tests),
  then run `npm test`, `npm run test:browser`, and `npm run build` in `installer/`.
- For documentation or Codex configuration changes, check the relevant syntax,
  links, CLI behavior, and `git diff --check`; do not rebuild or actuate hardware
  solely for a model or instruction edit.
- Match testing to the change. Add meaningful regressions for behavior fixes;
  broaden or repeat checks when a failure or concrete unresolved concern
  warrants it. Once required checks pass, finish the task rather than adding
  unrelated validation.

## Hardware and operational evidence

- Routine local development does not authorize live relay commands, point
  overrides, firmware deployment, reboots, or broad network scans. Use the
  current conversation's device scope and operating limits for authorized
  field work; historical bench reports do not establish current authority.
- The full relay HIL suite requires disconnected loads. For an authorized
  live-system test, use a scoped test with recorded baseline, bounded actuation,
  and cleanup; do not falsely assert that loads are disconnected.
- Preserve command ownership and restore only the test's temporary state.
  Do not clear other writers' priority slots or enable automatic relay-state
  restoration as a shortcut to repair missing BAS commands.
- Keep commissioning keys, credentials, raw site captures, and deployment
  evidence outside tracked files. Use placeholders in reusable instructions.
- Distinguish a displayed BAS value, a received BACnet command, an ACK, the
  winning priority, the relay-driver result, and physical contact feedback.
  This board has no independent relay contact feedback. Local tests or a built
  image do not prove that a field fix was installed or that a door moved.

## Communication

Lead with the result. Use concise, plain language and lists only when useful.
Report what changed, what was actually verified, and what remains uncertain.
Separate reproduced failure mechanisms from causes proven at the installation.
Explain concrete blockers or material risks without boilerplate warnings.
