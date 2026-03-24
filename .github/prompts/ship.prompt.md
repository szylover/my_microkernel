---
description: "Merge checklist + git workflow. Use when user says 提交/commit/push/PR/merge/发布/ship."
mode: "agent"
tools: [read, edit, search, execute]
---

Run the full pre-merge checklist and git workflow for this project.

## Steps

1. **Build verification**: Run `make iso` in `src/` — must pass
2. **Checklist** — verify each item and fix if needed:
   - [ ] `docs/changelog.md` — append today's entry if files/interfaces/behavior changed
   - [ ] `docs/roadmap.md` — update stage status markers (✅) if a stage was completed
   - [ ] `compile_commands.json` — run `make compdb` if any `.c`/`.asm` files were added/removed
   - [ ] `book/main.pdf` — run `make` in `book/` if any `.tex` files were modified
   - [ ] `.github/copilot-instructions.md` project tree — update if directory structure changed
3. **Git workflow**:
   - Create branch: `stage-N/short-description` from `main`
   - `git add -A && git commit -m "stage N: short english description"`
   - `git push -u origin <branch>`
   - `gh pr create --base main --head <branch> --title "same as commit" --body "中文改动要点"`
4. **If user also said merge**: `gh pr merge <number> --squash --delete-branch`
5. **Output**: PR link to user
