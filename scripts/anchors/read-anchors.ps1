# read-anchors.ps1 -- list every anchor as name + priority + status.
#
# A LAUNCHER, NOT AN IMPLEMENTATION -- and that is the whole design. The operator asked
# for a .sh and a .ps1 per verb; this repository's rule says a pair must not drift, while
# conceding that no gate can decide whether two arbitrary programs are equivalent. Both
# are satisfied by ONE implementation with eight entry points: the pair EXISTS on both
# hosts, takes the same flags and returns the same exit codes, and CANNOT diverge in
# behaviour because there is only one behaviour. A second hand-written markdown-table
# writer in PowerShell is exactly the class of defect anchors.py exists to prevent.
#
# Every argument is passed through untouched. See: anchors.py --help
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$py = $env:PYTHON
if (-not $py) {
    foreach ($c in @('python3', 'python', 'py')) {
        if (Get-Command $c -ErrorAction SilentlyContinue) { $py = $c; break }
    }
}
if (-not $py) {
    Write-Error "read-anchors: no python3, python or py on PATH. This is a launcher over $PSScriptRoot\anchors.py -- set \:PYTHON or install one."
    exit 3
}
& $py (Join-Path $PSScriptRoot 'anchors.py') list @args
exit $LASTEXITCODE
