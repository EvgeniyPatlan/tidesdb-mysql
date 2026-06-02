# Release automation — setup

Two-workflow setup so that new upstream TidesDB releases get caught fast but
publishing always happens behind a one-click manual trigger:

| Workflow | Trigger | Runner | What it does |
|----------|---------|--------|--------------|
| **`check-upstream-tidesdb.yml`** | daily @ 04:00 UTC + manual | GitHub-hosted (free) | Diffs `Dockerfile.mysql` against `gh api repos/tidesdb/tidesdb/releases/latest`; opens an issue when there's something to bump (labelled `upstream-bump`; also `needs-plugin-update` if `tidesdb.h` adds new public enums / error codes). |
| **`release.yml`** | manual `workflow_dispatch` only | self-hosted | Runs `scripts/release-upstream-bump.sh` end-to-end on your hardware. Builds the three images, runs the four validation gates, commits, tags, pushes Docker Hub, creates the GitHub release. |

The detection workflow is unattended; the release workflow is one click.

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

### 2. Docker Hub credentials

Repo → **Settings → Secrets and variables → Actions → New repository secret**,
add two secrets the release workflow expects:

| Secret | Value |
|--------|-------|
| `DOCKERHUB_USERNAME` | the Docker Hub user that can push to `perconalab/tidesdb-mysql` + `evgeniypatlan/test-images` |
| `DOCKERHUB_TOKEN`    | a Docker Hub **access token** (Settings → Security → New Access Token, scope: Read & Write) — *not* the account password |

`GITHUB_TOKEN` is injected automatically by Actions — no manual setup needed
for the tag push and `gh release create`.

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
   - pushes main and the tag, pushes Docker Hub images
     (`perconalab/tidesdb-mysql:<plugin>` + `:latest` and
     `evgeniypatlan/test-images:mysql-9.7-tidesdb-v<plugin>`),
   - creates the GitHub release.
   The job log uploads all per-stage logs as a `release-logs-<run-id>` artifact
   (retained 60 days), and you can review the validation report and Docker
   digests there if anything looks off.

   If any gate fails the script bails before publish — no commits are pushed,
   no images go out, the run fails the workflow and the artifact carries the
   failure logs for triage.

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
