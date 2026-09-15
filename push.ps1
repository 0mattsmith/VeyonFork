<#
.SYNOPSIS
    Create the VeyonFork GitHub repository (first run) and push changes (every run).

.DESCRIPTION
    First run  : creates a GitHub repo via the 'gh' CLI, wires it up as a separate
                 remote (default 'fork'), and pushes the working branch.
    Every run  : stages, commits with your label, and pushes.

    'origin' is deliberately left pointing at upstream veyon/veyon so you can keep
    pulling their updates. Your code goes to the 'fork' remote instead.

.PARAMETER Label
    The commit message ("custom push label"). Everything after the script name is
    treated as the label, so quotes are optional. Defaults to a timestamped message.

.EXAMPLE
    .\push.ps1 "Add lockable audio control"

.EXAMPLE
    .\push.ps1 Fix file browser download progress
    # quotes optional - remaining arguments are joined

.EXAMPLE
    .\push.ps1 -Private -EnableCI "Initial push"
    # first run: creates a private repo and enables the Windows CI workflow

.EXAMPLE
    .\push.ps1 -DryRun "test"
    # show what would happen, change nothing
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]] $Label,

    [string] $RepoName = 'VeyonFork',
    [string] $Remote   = 'fork',
    [string] $Branch   = 'veyonfork',

    # Only used when the repo is first created:
    [switch] $Private,

    # Copy docs/ci/build-windows.yml into .github/workflows/ if not already there
    [switch] $EnableCI,

    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

# ---------- helpers ----------------------------------------------------------

function Write-Step { param([string]$m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "    $m" -ForegroundColor Yellow }
function Fail       { param([string]$m) Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }

# Run git, throw on non-zero exit.
function Git-Run {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $GitArgs)
    if ($DryRun) { Write-Host "    [dry-run] git $($GitArgs -join ' ')" -ForegroundColor DarkGray; return }
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { Fail "git $($GitArgs -join ' ') failed (exit $LASTEXITCODE)" }
}

# Run git, capture stdout, ignore failure (for queries).
function Git-Try {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $GitArgs)
    $out = & git @GitArgs 2>$null
    return @{ Ok = ($LASTEXITCODE -eq 0); Out = ($out | Out-String).Trim() }
}

# ---------- preflight --------------------------------------------------------

Write-Step 'Checking environment'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "git not found on PATH. Install it with:  winget install Git.Git"
}

$inRepo = Git-Try rev-parse --is-inside-work-tree
if (-not $inRepo.Ok) { Fail "Not inside a git repository. Run this from your VeyonFork folder." }

# Always operate from the repository root, wherever the script was invoked from.
$repoRoot = (Git-Try rev-parse --show-toplevel).Out
Set-Location $repoRoot
Write-Ok "Repository: $repoRoot"

# ---------- branch -----------------------------------------------------------

$current = (Git-Try rev-parse --abbrev-ref HEAD).Out
if ($current -ne $Branch) {
    $exists = Git-Try rev-parse --verify --quiet "refs/heads/$Branch"
    if ($exists.Ok) {
        Write-Warn "Switching from '$current' to '$Branch'"
        Git-Run checkout $Branch
    }
    else {
        Write-Warn "Creating branch '$Branch' from '$current'"
        Git-Run checkout -b $Branch
    }
}
Write-Ok "Branch: $Branch"

# ---------- optional: enable the Windows CI workflow -------------------------

if ($EnableCI) {
    Write-Step 'Enabling Windows CI workflow'
    $src = Join-Path $repoRoot 'docs\ci\build-windows.yml'
    $dstDir = Join-Path $repoRoot '.github\workflows'
    $dst = Join-Path $dstDir 'build-windows.yml'

    if (-not (Test-Path $src)) {
        Write-Warn "Not found: docs\ci\build-windows.yml - skipping"
    }
    elseif (Test-Path $dst) {
        Write-Ok 'Already enabled'
    }
    else {
        if ($DryRun) { Write-Host "    [dry-run] copy -> .github\workflows\build-windows.yml" -ForegroundColor DarkGray }
        else {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
            Copy-Item $src $dst
            Write-Ok 'Copied to .github\workflows\build-windows.yml'
        }
    }
}

# ---------- commit label -----------------------------------------------------

if ($Label -and $Label.Count -gt 0) {
    $message = ($Label -join ' ').Trim()
}
else {
    $message = "VeyonFork update - $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
    Write-Warn "No label given, using: $message"
}

# ---------- create repo on first run -----------------------------------------

$remoteUrl = Git-Try remote get-url $Remote

if (-not $remoteUrl.Ok) {
    Write-Step "Remote '$Remote' not found - first run, creating GitHub repository"

    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        Write-Host ''
        Write-Host '  The GitHub CLI is needed to create the repo automatically.' -ForegroundColor Yellow
        Write-Host '    winget install GitHub.cli'
        Write-Host '    gh auth login'
        Write-Host ''
        Write-Host '  Or create the repo yourself on github.com, then run:' -ForegroundColor Yellow
        Write-Host "    git remote add $Remote https://github.com/<you>/$RepoName.git"
        Write-Host '    .\push.ps1 "your label"'
        Fail 'gh not found on PATH.'
    }

    & gh auth status 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Not logged in to GitHub. Run:  gh auth login" }

    $visibility = if ($Private) { '--private' } else { '--public' }
    Write-Ok "Creating $visibility repository '$RepoName'"
    if ($Private) {
        Write-Warn 'Note: private repos consume Actions minutes, and Windows runners bill at 2x.'
    }

    if ($DryRun) {
        Write-Host "    [dry-run] gh repo create $RepoName --source=. --remote=$Remote $visibility" -ForegroundColor DarkGray
    }
    else {
        # --source=. creates a standalone repo from this checkout (not a GitHub
        # "fork"). That matters: Actions are enabled by default on a normal repo,
        # whereas forks require you to click through a prompt to enable workflows.
        & gh repo create $RepoName --source=. --remote=$Remote $visibility
        if ($LASTEXITCODE -ne 0) { Fail 'gh repo create failed.' }
        Write-Ok "Remote '$Remote' added"
        Write-Warn 'First push includes full upstream history (~55 MB) - this one will take a while.'
    }
}
else {
    Write-Step "Remote '$Remote' -> $($remoteUrl.Out)"
}

# ---------- stage & commit ---------------------------------------------------

Write-Step 'Staging changes'
Git-Run add -A

$status = Git-Try status --short
if ([string]::IsNullOrWhiteSpace($status.Out)) {
    Write-Ok 'Working tree clean - nothing new to commit'
}
else {
    $lines = ($status.Out -split "`n")
    Write-Ok "$($lines.Count) file(s) changed:"
    $lines | Select-Object -First 15 | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkGray }
    if ($lines.Count -gt 15) { Write-Host "      ... and $($lines.Count - 15) more" -ForegroundColor DarkGray }

    Write-Step "Committing: $message"
    Git-Run commit -m $message
    Write-Ok 'Committed'
}

# ---------- push -------------------------------------------------------------

Write-Step "Pushing to $Remote/$Branch"
Git-Run push -u $Remote $Branch
Write-Ok 'Push complete'

# ---------- summary ----------------------------------------------------------

if (-not $DryRun) {
    $url = (Git-Try remote get-url $Remote).Out -replace '\.git$', '' -replace '^git@github\.com:', 'https://github.com/'
    Write-Host ''
    Write-Host 'Done.' -ForegroundColor Green
    Write-Host "  Repository : $url"
    Write-Host "  Actions    : $url/actions" -ForegroundColor Cyan
    Write-Host ''
    Write-Host '  Windows binaries appear as a run artifact once the build finishes.' -ForegroundColor DarkGray
}
