<#
.SYNOPSIS
    Create the VeyonFork GitHub repository (first run), push changes, and
    optionally watch the CI build and download the Windows binaries.

.DESCRIPTION
    First run  : creates a GitHub repo via 'gh', wires it up as a separate
                 remote (default 'fork'), and pushes the working branch.
    Every run  : stages, commits with your label, pushes.
    With -Get  : waits for the Windows CI build and downloads the binaries.

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
    .\push.ps1 -Get "Try the audio plugin"
    # push, wait for CI, download the Windows .exe/.dll into .\artifacts\

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

    # Wait for the CI run to finish (streams status)
    [switch] $Watch,

    # Wait for CI, then download the build artifacts. Implies -Watch.
    [switch] $Get,

    [string] $ArtifactDir = 'artifacts',

    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
if ($Get) { $Watch = $true }

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
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Fail 'gh not found on PATH. Install it with:  winget install GitHub.cli'
}

gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail 'Not logged in to GitHub. Run:  gh auth login' }
Write-Ok 'git + gh ready'

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

# ---------- keep downloaded artifacts out of git -----------------------------
# Without this, 'git add -A' would happily commit the .exe/.dll you just pulled.

$gitignore = Join-Path $repoRoot '.gitignore'
$ignoreLine = "$ArtifactDir/"
$hasIgnore = (Test-Path -LiteralPath $gitignore) -and
             ((Get-Content -LiteralPath $gitignore) -contains $ignoreLine)
if (-not $hasIgnore) {
    if ($DryRun) { Write-Dim "[dry-run] append '$ignoreLine' to .gitignore" }
    else {
        Add-Content -LiteralPath $gitignore -Value "`n# Downloaded CI build artifacts`n$ignoreLine"
        Write-Ok "Added '$ignoreLine' to .gitignore"
    }
}

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

$remoteUrl  = (git remote get-url $Remote 2>$null | Out-String).Trim()
$haveRemote = ($LASTEXITCODE -eq 0)

if (-not $haveRemote) {
    Write-Step "Remote '$Remote' not found - first run, creating GitHub repository"

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

$committed = $false
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
    $committed = $true
    Write-Ok 'Committed'
}

# ---------- push -------------------------------------------------------------

Write-Step "Pushing to $Remote/$Branch"
Invoke-GitChange @('push', '-u', $Remote, $Branch)
Write-Ok 'Push complete'

# ---------- watch CI / fetch binaries ----------------------------------------

if ($Watch -and -not $DryRun) {
    if (-not $committed) {
        Write-Warn 'No new commit, so no new run was triggered - using the most recent run.'
    }

    Write-Step 'Locating CI run'
    # Give GitHub a moment to register the run this push just triggered.
    Start-Sleep -Seconds 5

    $runId = (gh run list --branch $Branch --limit 1 --json databaseId --jq '.[0].databaseId' 2>$null | Out-String).Trim()

    if ([string]::IsNullOrWhiteSpace($runId)) {
        Write-Warn 'No workflow runs found.'
        Write-Dim 'Is the workflow enabled? Try:  .\push.ps1 -EnableCI "enable ci"'
    }
    else {
        Write-Ok "Run #$runId"
        Write-Dim 'Watching - Ctrl+C is safe, the build keeps going on GitHub.'

        gh run watch $runId --exit-status
        $buildOk = ($LASTEXITCODE -eq 0)

        if (-not $buildOk) {
            Write-Warn 'Build failed.'
            Write-Dim "Logs:  gh run view $runId --log-failed"
            Write-Dim "Web :  gh run view $runId --web"
        }
        elseif ($Get) {
            Write-Step "Downloading artifacts into .\$ArtifactDir\"
            New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
            gh run download $runId --dir $ArtifactDir
            if ($LASTEXITCODE -ne 0) { Write-Warn 'Download failed (artifacts may have expired).' }
            else {
                $files = @(Get-ChildItem -Path $ArtifactDir -Recurse -File -Include *.exe, *.dll -ErrorAction SilentlyContinue)
                Write-Ok "$($files.Count) binaries downloaded"
                $files | Select-Object -First 12 | ForEach-Object { Write-Dim "  $($_.Name)" }
                if ($files.Count -gt 12) { Write-Dim "  ... and $($files.Count - 12) more" }
            }
        }
        else {
            Write-Ok 'Build succeeded.'
            Write-Dim "Fetch the binaries with:  gh run download $runId --dir $ArtifactDir"
        }
    }
}

# ---------- summary ----------------------------------------------------------

if (-not $DryRun) {
    $url = (git remote get-url $Remote | Out-String).Trim()
    $url = $url -replace '\.git$', '' -replace '^git@github\.com:', 'https://github.com/'
    Write-Host ''
    Write-Host 'Done.' -ForegroundColor Green
    Write-Host "  Repository : $url"
    Write-Host "  Actions    : $url/actions" -ForegroundColor Cyan
    if (-not $Watch) {
        Write-Host ''
        Write-Dim 'Tip: add -Get to wait for the build and pull the binaries down automatically.'
    }
}
