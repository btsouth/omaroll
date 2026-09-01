# omaroll

Capture library for Omarchy. See PLAN.md for the full spec.

## Rules

- **GitHub account: always `tsouth89`.** Never `bts-cssi`. Check with `gh auth status` before any
  `gh` command in this repo and switch with `gh auth switch -u tsouth89` if it is not active.
- **Nothing public until told otherwise.** No remote, no push, no release, no posts. Local only.
- Stack is C++20 + Qt 6.8 Quick + CMake/Ninja, matching omakade and the Omarchy first-party apps.
- Reuse the omakade skeleton (`~/Projects/steam-launcher`) rather than starting fresh. The lift
  table is in PLAN.md section 6.
- Never move, rename, or rewrite a user's capture file. Actions write new files beside originals.
- Before adding a module, check the rule in PLAN.md section 4: if a package already in
  `omarchy-base.packages` can do it, it is a row in the action matrix, not code.
