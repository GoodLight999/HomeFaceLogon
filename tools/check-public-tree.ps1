[CmdletBinding()]
param(
    [switch]$IncludeUntracked
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-RepositoryFiles {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git -and (Test-Path '.git')) {
        $files = @(& git ls-files)
        if ($LASTEXITCODE -ne 0) {
            throw 'git ls-files failed.'
        }

        if ($IncludeUntracked) {
            $untracked = @(& git ls-files --others --exclude-standard)
            if ($LASTEXITCODE -ne 0) {
                throw 'git ls-files --others failed.'
            }
            $files += $untracked
        }

        return $files | Where-Object { $_ } | Sort-Object -Unique
    }

    return Get-ChildItem -Recurse -File |
        ForEach-Object {
            [IO.Path]::GetRelativePath((Get-Location).Path, $_.FullName)
        }
}

$forbiddenPathPatterns = @(
    '(^|/)(local|private|secrets|runtime-data|diagnostics|logs|captures|screenshots|test-results|artifacts|sandbox)(/|$)',
    '(^|/)(secret|face)\.bin$',
    '\.(pfx|p12|p8|key|pem|snk|pvk|jks|keystore)$',
    '\.(dmp|mdmp|etl|evtx|wer|trace|log)$',
    '\.(export|backup|machine)\.reg$',
    '\.(dpapi|credential|credentials|face-template|embedding|embeddings)$',
    '(^|/)config\.(local|private)\.json$',
    '\.(local|private)\.(json|ya?ml|toml|ini)$'
)

$hardContentPatterns = [ordered]@{
    'Private key marker' = '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----'
    'Real-looking Windows SID' = 'S-1-5-21-(?!0000000000-0000000000-0000000000)[0-9]{6,}-[0-9]{6,}-[0-9]{6,}-[0-9]{3,}'
    'Microsoft account qualified name' = 'MicrosoftAccount\\[^\s"''<>]+@[^\s"''<>]+'
    'USB camera symbolic link' = '\\\\\?\\(usb|hid)#vid_[0-9a-f]{4}&pid_[0-9a-f]{4}'
}

$textExtensions = @(
    '.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inl',
    '.cs', '.ps1', '.psm1', '.cmd', '.bat',
    '.json', '.xml', '.yml', '.yaml', '.toml', '.ini',
    '.md', '.txt', '.reg', '.props', '.targets', '.cmake'
)

$violations = New-Object System.Collections.Generic.List[string]
$files = @(Get-RepositoryFiles)

foreach ($relativePath in $files) {
    $normalized = $relativePath.Replace('\', '/')

    foreach ($pattern in $forbiddenPathPatterns) {
        if ($normalized -match $pattern) {
            $violations.Add("Forbidden tracked path: $relativePath")
            break
        }
    }

    $fullPath = Join-Path (Get-Location) $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        continue
    }

    $extension = [IO.Path]::GetExtension($relativePath).ToLowerInvariant()
    if ($textExtensions -notcontains $extension) {
        continue
    }

    # Example files and the policy/checker itself may intentionally contain dummy patterns.
    if ($normalized -match '(^|/)(config\.example\.json|public-repository-policy\.md|check-public-tree\.ps1)$') {
        continue
    }

    $content = Get-Content -LiteralPath $fullPath -Raw -ErrorAction Stop
    foreach ($entry in $hardContentPatterns.GetEnumerator()) {
        if ($content -match $entry.Value) {
            $violations.Add("$($entry.Key) found in: $relativePath")
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host 'Public-tree check failed:' -ForegroundColor Red
    $violations | Sort-Object -Unique | ForEach-Object {
        Write-Host "  - $_" -ForegroundColor Red
    }
    exit 1
}

Write-Host "Public-tree check passed. Inspected $($files.Count) file(s)." -ForegroundColor Green
