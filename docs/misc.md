---
layout: default
title: Misc
---

# Miscellaneous

# 1. Docs deploy workflow (`.github/workflows/pages.yml`)

The GitHub Pages site deploys via Actions so the gitignored Doxygen
output (`docs/html/`) is generated before the Jekyll build — the stock
docs/-folder Pages build cannot run `make doxygen`. The workflow is
inert until the Pages source setting flips from the docs/ folder to
GitHub Actions; both modes serve the same `baseurl: "/avrmem"` site.

- Trigger: pushes to `master` touching `docs/**`, `Doxyfile`, `src/**`,
  `include/**`, `README.md`, `Makefile`, or the workflow itself, plus
  manual `workflow_dispatch`. Test-only commits skip the docs build.
- Build: recursive checkout → apt doxygen/graphviz → `make doxygen` →
  bundler-cached Ruby → `jekyll build` into `docs/_site` → Pages
  artifact → `deploy-pages` (OIDC, `github-pages` environment).
- Every build-run command is locally reproducible (`make doxygen`,
  `bundle exec jekyll build`); only apt versions float with the runner
  image. Keep docs-affecting changes green here the same way as the
  test suite.
