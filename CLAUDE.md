# Ultimatrix

## Session start (default for every new chat)
Read `HANDOFF.txt` and `.claude/filemap.md` first, then only the files the task needs.

## Handoff file (mandatory)
`HANDOFF.txt` is the shared handoff between sessions and agents (Claude, Codex).
- At the start of every session on this project, read `HANDOFF.txt` first.
- Before finishing any turn that changed files, config, or decisions, append a short dated entry to the end of `HANDOFF.txt` in the existing style (`Claude (13 Oct, later) TOPIC: what changed, files touched, what was/wasn't verified, what's still open`), and update the `Updated:` line at the top.
- Do this automatically without being asked. Skip only for turns that changed nothing.
- Keep entries brief; never put secrets in it.
- Keep the file compact (about 75 lines): when appending, prune finished/superseded/stale content (done in-progress notes, resolved blockers, obsolete decisions, stale commands). Fold history into a short summary. Keep current state, still-valid decisions and open items only.

## File map
`.claude/filemap.md` indexes the repo (path + purpose per file/dir). Check it before exploring; read only the files the task needs. Update it when files are added, removed or repurposed.

@.claude/filemap.md
