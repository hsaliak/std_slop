# Name: github-pages
# Description: Design, generate, publish, and verify a static GitHub Pages site for any repository using GitHub Actions and gh.

# GitHub Pages

## Purpose

Use this skill to turn repository content into a static website and publish it with GitHub Pages. The skill supports inspection, design, and publication. It must keep site generation grounded in repository content and must not publish or overwrite remote configuration without approval.

## Modes

Select one mode from the user request:

- `inspect`: perform read-only repository, Pages, workflow, and deployment inspection.
- `design`: inspect the repository and produce a site plan without writing files or changing GitHub.
- `publish`: design, generate, validate, commit, push, configure Pages, and verify deployment.

If the mode is not clear, use `inspect` and ask whether the user wants `design` or `publish`.

## Defaults

- Deployment: GitHub Actions.
- Site format: plain static HTML and CSS.
- Artifact directory: `site/`.
- Workflow: `.github/workflows/pages.yml`.
- Source branch: repository default branch.
- JavaScript: none unless the user requires it.

A repository may override these defaults. Do not introduce Jekyll, Ruby, Node, or a frontend framework unless the user requests it or the repository already uses it.

## Required safety rules

1. Inspect before writing.
2. Stop if the working tree is dirty unless the user identifies which changes belong to the site.
3. Do not delete or replace an existing site directory, Pages workflow, `CNAME`, or Pages configuration without explicit approval.
4. Do not create a repository, change visibility, change DNS, or modify organization settings.
5. Do not commit secrets, tokens, local paths, generated credentials, or private repository content.
6. Do not claim publication until the Pages deployment is successful and its URL is verified.
7. Ask for approval before committing, pushing, configuring Pages, or changing an existing deployment.
8. Use `gh api` for Pages configuration. Never use browser automation as a substitute.

## Inputs

Collect or infer these values:

```text
repository
mode: inspect | design | publish
site purpose and audience
content sources
pages and navigation
output directory
source branch
build or validation command
custom domain, if any
```

For `publish`, confirm inferred values before changing files or GitHub state.

## Inspect mode

Run read-only checks:

```bash
git status --short
gh repo view --json nameWithOwner,defaultBranchRef,isPrivate,homepageUrl
find . -maxdepth 3 -type f | sort
gh workflow list
gh api repos/{owner}/{repo}/pages
```

A missing Pages site is an expected `404`; report it as `not configured`, not as an unexplained command failure. Inspect these paths before proposing changes:

```text
site/
docs/
.github/workflows/
CNAME
```

Also inspect existing workflows and recent deployment runs:

```bash
gh run list --limit 10
gh api repos/{owner}/{repo}/pages/deployments
```

Report:

```text
Repository:
Default branch:
Working tree:
Existing site source:
Pages configuration:
Pages workflow:
Recent deployment:
Custom domain:
Recommended next mode:
```

## Design mode

Produce a concise plan before generating files:

```text
Purpose:
Audience:
Pages:
Navigation:
Content sources:
Visual system:
Responsive behavior:
Output directory:
Deployment branch:
Validation command:
Files to create or modify:
```

Use repository documentation and source as the content authority. Do not invent product behavior, commands, APIs, or claims. Keep the site formal and technical.

The default page structure is only a starting point:

```text
site/
  index.html
  404.html
  styles.css
  pages/
  assets/
```

Do not create pages that have no identified content source or user requirement.

## Publish mode

Execute these phases in order.

### 1. Inspect

Complete the inspect-mode checks. If an existing Pages workflow, site, custom domain, or deployment is found, explain the proposed changes and request approval before modifying it.

### 2. Design

Produce the site plan. For a documentation site, prefer a small number of pages with consistent navigation over copying every Markdown file into a page.

### 3. Generate

Create plain static artifacts with these requirements:

- semantic HTML;
- a title on every page;
- consistent navigation and active-page indication;
- responsive layout;
- keyboard-accessible navigation;
- readable code blocks;
- relative internal links;
- no unnecessary client-side JavaScript;
- no remote dependency unless approved;
- a `404.html` page;
- assets contained within the site output directory.

Keep generated artifacts separate from source Markdown unless the user explicitly chooses `docs/` as the publish directory.

### 4. Validate

Before commit, verify:

- every internal link resolves;
- every referenced asset exists;
- every page has a title;
- navigation is consistent;
- no generated file contains secrets or machine-specific paths;
- the output directory contains only intended artifacts;
- the site can be served locally by a static server;
- the repository's relevant tests and checks pass.

Use a local server for a manual smoke check, for example:

```bash
python3 -m http.server 8000 --directory site
```

Do not treat a local server as a substitute for repository validation.

### 5. Add or update the workflow

For the default Actions deployment, create `.github/workflows/pages.yml` only after checking that no conflicting workflow exists.

Use the current official Pages actions and pin their major versions unless the repository has a stronger version policy:

```yaml
name: Deploy Pages

on:
  push:
    branches: [main]
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: pages
  cancel-in-progress: true

jobs:
  deploy:
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/configure-pages@v5
      - uses: actions/upload-pages-artifact@v3
        with:
          path: site
      - id: deployment
        uses: actions/deploy-pages@v4
```

If the repository needs a build step, place it between checkout and artifact upload. Confirm the command and output directory before adding it.

### 6. Commit and push approval

Before making remote changes, show:

```text
Files to create:
Files to modify:
Files to delete:
Commit message:
Source branch:
Push target:
Pages configuration change:
Expected URL:
```

Wait for approval. Then create one focused commit and push it to the selected branch. Do not combine unrelated code changes with the site commit.

### 7. Configure Pages with gh

Inspect the current state first:

```bash
gh api repos/{owner}/{repo}/pages
```

For a new Actions-backed Pages site, configure the workflow build type. Write this request body to a temporary, untracked file and remove it after the request:

```json
{
  "build_type": "workflow"
}
```

Create the site with the Pages REST API:

```bash
gh api \
  --method POST \
  repos/{owner}/{repo}/pages \
  --input pages-config.json
```

The API also supports legacy branch publishing with a `source` object containing `branch` and `path` (`/` or `/docs`), but do not select that mode unless the user explicitly requests it. For an existing site, use `PUT` only after comparing the current configuration and receiving approval. Preserve an existing custom domain unless the user explicitly requests a change.

Do not assume a successful API response means the site is deployed. Continue to deployment verification.

### 8. Verify deployment

After the push, inspect the workflow:

```bash
gh run list --workflow pages.yml --limit 5
gh run watch <run-id>
```

Then inspect Pages:

```bash
gh api repos/{owner}/{repo}/pages
gh api repos/{owner}/{repo}/pages/deployments
```

Publication succeeds only when:

- the workflow completed successfully;
- the Pages deployment is successful or active;
- the Pages response includes the expected URL;
- the URL is reported to the user.

On failure, report the workflow URL, failing job, and relevant API status. Do not retry destructive configuration changes automatically.

## Branch publishing

Actions publishing is the default. If the user explicitly selects branch publishing, inspect the current Pages API schema and configure the requested branch and path only after approval. Do not silently convert an existing branch deployment to Actions or the reverse.

## Custom domains

Treat `CNAME` and Pages custom-domain settings as protected state:

- inspect both repository content and Pages configuration;
- ask before creating, replacing, or deleting `CNAME`;
- do not change DNS records;
- verify the configured URL after deployment.

## Failure handling

- Missing Pages configuration: continue in `design`, or request publish approval in `publish`.
- Dirty working tree: stop and identify conflicting changes.
- Existing workflow: inspect and propose a minimal update; do not overwrite.
- Existing custom domain: preserve it unless explicitly changed.
- API `401`/`403`: report authentication or permission requirements and stop.
- API `404` for Pages inspection: treat as not configured.
- Failed workflow: report run details and do not claim deployment.
- Ambiguous content requirements: remain in `design` and ask one focused question.

## Output contract

At the end of each mode, report:

```text
mode:
status: inspected | designed | published | blocked
repository:
files changed:
commit:
workflow run:
pages status:
url:
next action:
```

For `inspect` and `design`, `commit`, `workflow run`, and `url` may be `not applicable`. For `publish`, do not report `published` without deployment evidence.
