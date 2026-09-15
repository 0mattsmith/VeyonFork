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
    # first run: create a private repo and enable the Windows CI workflow

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

function Write-Step { param([string]$Message) Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Write-Ok   { param([string]$Message) Write-Host "    $Message" -ForegroundColor Green }
function Write-Warn { param([string]$Message) Write-Host "    $Message" -ForegroundColor Yellow }
function Write-Dim  { param([string]$Message) Write-Host "    $Message" -ForegroundColor DarkGray }
function Fail       { param([string]$Message) Write-Host "`nERROR: $Message" -ForegroundColor Red; exit 1 }

# Mutating git call. Arguments are passed as a single array so PowerShell never
# tries to interpret '--flag' tokens as parameter names.
function Invoke-GitChange {
    param([string[]] $Arguments)
    if ($DryRun) { Write-Dim "[dry-run] git $($Arguments -join ' ')"; return }
    & git @Arguments
    if ($LASTEXITCODE -ne 0) { Fail "git $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
}

# ---------- preflight --------------------------------------------------------

Write-Step 'Checking environment'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail 'git not found on PATH. Install it with:  winget install Git.Git'
}

git rev-parse --is-inside-work-tree 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail 'Not inside a git repository. Run this from your VeyonFork folder.'
}

# Always operate from the repository root, wherever the script was invoked from.
$repoRoot = (git rev-parse --show-toplevel | Out-String).Trim()
Set-Location -LiteralPath $repoRoot
Write-Ok "Repository: $repoRoot"

# ---------- branch -----------------------------------------------------------

$current = (git rev-parse --abbrev-ref HEAD | Out-String).Trim()
if ($current -ne $Branch) {
    git rev-parse --verify --quiet "refs/heads/$Branch" 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Warn "Switching from '$current' to '$Branch'"
        Invoke-GitChange @('checkout', $Branch)
    }
    else {
        Write-Warn "Creating branch '$Branch' from '$current'"
        Invoke-GitChange @('checkout', '-b', $Branch)
    }
}
Write-Ok "Branch: $Branch"

# ---------- optional: enable the Windows CI workflow -------------------------

if ($EnableCI) {
    Write-Step 'Enabling Windows CI workflow'
    $src    = Join-Path $repoRoot 'docs\ci\build-windows.yml'
    $dstDir = Join-Path $repoRoot '.github\workflows'
    $dst    = Join-Path $dstDir 'build-windows.yml'

    if (-not (Test-Path -LiteralPath $src)) {
        Write-Warn 'Not found: docs\ci\build-windows.yml - skipping'
    }
    elseif (Test-Path -LiteralPath $dst) {
        Write-Ok 'Already enabled'
    }
    elseif ($DryRun) {
        Write-Dim '[dry-run] copy -> .github\workflows\build-windows.yml'
    }
    else {
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst
        Write-Ok 'Copied to .github\workflows\build-windows.yml'
    }
}

# ---------- commit label -----------------------------------------------------

if ($Label -and $Label.Count -gt 0) {
    $message = ($Label -join ' ').Trim()
}
if ([string]::IsNullOrWhiteSpace($message)) {
    $message = "VeyonFork update - $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
    Write-Warn "No label given, using: $message"
}

# ---------- create repo on first run -----------------------------------------

$remoteUrl = (git remote get-url $Remote 2>$null | Out-String).Trim()
$haveRemote = ($LASTEXITCODE -eq 0)

if (-not $haveRemote) {
    Write-Step "Remote '$Remote' not found - first run, creating GitHub repository"

    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        Write-Host ''
        Write-Warn 'The GitHub CLI is needed to create the repo automatically:'
        Write-Host '      winget install GitHub.cli'
        Write-Host '      gh auth login'
        Write-Host ''
        Write-Warn 'Or create it yourself on github.com, then run:'
        Write-Host "      git remote add $Remote https://github.com/<you>/$RepoName.git"
        Write-Host '      .\push.ps1 "your label"'
        Fail 'gh not found on PATH.'
    }

    gh auth status 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 'Not logged in to GitHub. Run:  gh auth login' }

    $visibility = if ($Private) { '--private' } else { '--public' }
    Write-Ok "Creating $visibility repository '$RepoName'"
    if ($Private) {
        Write-Warn 'Note: private repos consume Actions minutes, and Windows runners bill at 2x.'
    }

    if ($DryRun) {
        Write-Dim "[dry-run] gh repo create $RepoName --source=. --remote=$Remote $visibility"
    }
    else {
        # --source=. creates a standalone repo from this checkout rather than a
        # GitHub "fork". That matters: Actions run by default on a normal repo,
        # whereas forks make you click through a prompt before workflows run.
        gh repo create $RepoName --source=. --remote=$Remote $visibility
        if ($LASTEXITCODE -ne 0) { Fail 'gh repo create failed.' }
        Write-Ok "Remote '$Remote' added"
        Write-Warn 'First push carries full upstream history (~55 MB) - it will take a while.'
    }
}
else {
    Write-Step "Remote '$Remote' -> $remoteUrl"
}

# ---------- stage & commit ---------------------------------------------------

Write-Step 'Staging changes'
Invoke-GitChange @('add', '-A')

$status = (git status --short | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($status)) {
    Write-Ok 'Working tree clean - nothing new to commit'
}
else {
    $lines = @($status -split "`r?`n")
    Write-Ok "$($lines.Count) file(s) changed:"
    $lines | Select-Object -First 15 | ForEach-Object { Write-Dim "  $_" }
    if ($lines.Count -gt 15) { Write-Dim "  ... and $($lines.Count - 15) more" }

    Write-Step "Committing: $message"
    Invoke-GitChange @('commit', '-m', $message)
    Write-Ok 'Committed'
}

# ---------- push -------------------------------------------------------------

Write-Step "Pushing to $Remote/$Branch"
Invoke-GitChange @('push', '-u', $Remote, $Branch)
Write-Ok 'Push complete'

# ---------- summary ----------------------------------------------------------

if (-not $DryRun) {
    $url = (git remote get-url $Remote | Out-String).Trim()
    $url = $url -replace '\.git$', '' -replace '^git@github\.com:', 'https://github.com/'
    Write-Host ''
    Write-Host 'Done.' -ForegroundColor Green
    Write-Host "  Repository : $url"
    Write-Host "  Actions    : $url/actions" -ForegroundColor Cyan
    Write-Host ''
    Write-Dim 'Windows binaries appear as a run artifact once the build finishes.'
}
