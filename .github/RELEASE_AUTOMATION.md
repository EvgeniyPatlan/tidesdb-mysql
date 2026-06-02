# Release automation — setup

Three-stage flow that automates everything the repo can do safely while
keeping Docker Hub credentials on your workstation:

| Stage | Where | What it does |
|-------|-------|--------------|
| **1. Detect** — `.github/workflows/check-upstream-tidesdb.yml` | GitHub-hosted runner (free), daily @ 04:00 UTC + manual | Diffs `Dockerfile.mysql` against `gh api repos/tidesdb/tidesdb/releases/latest`; opens an issue when there's something to bump (`upstream-bump`; also `needs-plugin-update` if `tidesdb.h` adds new public enums / error codes). |
| **2. Prepare + tag** — `.github/workflows/release.yml` | self-hosted runner, manual `workflow_dispatch` | Runs `scripts/release-upstream-bump.sh`: bumps refs, rebuilds images, runs the four validation gates, writes the validation report, commits + tags, pushes main, pushes the tag. **Stops there.** |
| **3. Publish** — `scripts/release-docker.sh` | your workstation, manual | Tags and pushes the three Docker Hub destinations from the local `tidesdb/mysql:9.7` image and creates the GitHub release. Docker Hub credentials never leave the workstation. |

Detection is unattended; the prepare+tag workflow is one click in Actions; the
publish step is one command on your machine when you're ready to ship.

## One-time setup

### 1. Register a self-hosted runner

The validation gates need **~12 CPU / 32 GiB RAM / hundreds of GB disk** — the
GitHub-hosted runners can't run them. Your workstation works fine; you
register it once and it stays connected.

1. Repo → **Settings → Actions → Runners → New self-hosted runner**.
2. Follow the listed install instructions on the machine that ran the v0.3.x
   releases (or any equivalent box).
3. Make sure the runner user has access to docker (either via the `docker`
   group or `sg docker` — the release script auto-detects).
4. **Add a custom label `tidesdb-release`** when prompted (or via
   Settings → Runners → your-runner → Edit). The workflow's
   `runs-on: [self-hosted, linux, x64, tidesdb-release]` pins jobs to this
   runner so unrelated workflows can't accidentally schedule on your box.
5. Start the runner as a service so it survives reboots:
   `sudo ./svc.sh install && sudo ./svc.sh start` (in the runner install dir).

Security caveat: a self-hosted runner can execute arbitrary code from any
workflow that targets it. Single-author public repos are fine; in a
multi-contributor setting, lock the runner down via
**Settings → Actions → Require approval for first-time contributors** and
restrict who can trigger `workflow_dispatch`.

### 2. Docker Hub login on your workstation (one-time)

```
docker login
```

That's it — the credentials live on your machine, never in CI. The Release
workflow does not touch Docker Hub.

`GITHUB_TOKEN` for the workflow's git push + tag push is injected automatically
by Actions; `gh release create` is done by `release-docker.sh` on your machine
using your existing `gh auth`.

### 3. (Optional) Issue labels

Both workflows reference two labels for triage; create them once under
**Issues → Labels** if they don't already exist:

- `upstream-bump` — opened by the detection workflow for any new release.
- `needs-plugin-update` — added when `tidesdb.h` introduces new public enums
  or error codes that need a hand-edited `plugin/ha_tidesdb.cc` update before
  the release workflow can run.

## How a release happens after setup

1. **Detection workflow runs at 04:00 UTC.** If upstream has a new release and
   `tidesdb.h` is safe, you get a new issue titled *"Upstream TidesDB vX.Y.Z
   available — ready to bump"*. If the header has new error codes, you get a
   different issue tagged `needs-plugin-update` listing the additions, and
   nothing else happens until the plugin is updated.

2. **You click Actions → Release → Run workflow.** Inputs:
   - `tidesdb_version`: usually leave empty (uses the latest upstream); fill
     in `vX.Y.Z` to pin a specific version.
   - `plugin_version`: usually leave empty (auto-patch-bump from the latest
     `v*` tag); fill in `X.Y.Z` for a minor/major bump.
   - `skip_throughput`: leave **off** for a real release (the WARE=100 gate is
     load-bearing); only enable for a quick test build.

3. **The release workflow runs on your self-hosted runner.** It:
   - reruns the preflight checks,
   - rebuilds the three images (`tidesdb/mysql:9.7`, `tidesdb/mysql-mtr:9.7`,
     `tidesdb/mwbench:<engine>`),
   - runs MTR + mwbench + SIGKILL recovery + WARE=100 throughput,
   - writes `docs/<engine>-validation-report.md`,
   - updates CHANGELOG.md and KNOWN-ISSUES.md,
   - commits two themed commits and tags `v<plugin>`,
   - **pushes main and the tag, then stops.**
   All per-stage logs upload as a `release-logs-<run-id>` artifact (retained
   60 days).

   If any gate fails the script bails before any push — no commits go out, no
   tag is pushed, the workflow fails and the artifact carries the failure logs
   for triage.

4. **On your workstation, run the publish step:**
   ```
   git pull
   ./scripts/release-docker.sh
   ```
   The script verifies the local `tidesdb/mysql:9.7` image (still cached from
   the workflow's build), tags + pushes
   `perconalab/tidesdb-mysql:<plugin>` + `:latest` and
   `evgeniypatlan/test-images:mysql-9.7-tidesdb-v<plugin>`, then creates the
   GitHub release (whose notes reference the validation report and advertise
   the `docker pull` — which is now actually pullable). Flags:
   `--dry-run`, `--yes`, `--no-gh-release`, `--plugin=X.Y.Z`,
   `--tidesdb=vX.Y.Z` (versions auto-resolve from `Dockerfile.mysql` + the
   latest tag if you omit them).

## Running the detection workflow on demand

Either:
- **Actions → Check upstream TidesDB → Run workflow** in the UI, or
- `gh workflow run check-upstream-tidesdb.yml` from the CLI.

Useful when you've manually bumped or want to clear out a stale open issue
after a release.

## Disabling / pausing

To pause the daily check (e.g. while you're in the middle of a plugin
refactor that will conflict with an auto-bump), either:
- Disable the workflow via **Actions → Check upstream TidesDB → ⋯ → Disable
  workflow**, or
- Delete the `cron:` line from the workflow file (leaving `workflow_dispatch`
  for on-demand triggering).
