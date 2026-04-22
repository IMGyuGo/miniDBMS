param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [int]$ConcurrentRequests = 8,
    [switch]$Quiet,
    [switch]$PassThru
)

$ErrorActionPreference = "Stop"

function Write-Log {
    param([string]$Message = "")

    if (-not $Quiet) {
        Write-Host $Message
    }
}

function ConvertTo-MiniDbJsonString {
    param([string]$Value)

    $escaped = $Value.Replace('\', '\\')
    $escaped = $escaped.Replace('"', '\"')
    $escaped = $escaped.Replace("`r", '\r')
    $escaped = $escaped.Replace("`n", '\n')
    $escaped = $escaped.Replace("`t", '\t')

    return '"' + $escaped + '"'
}

function ConvertTo-MiniDbJson {
    param([hashtable]$Body)

    $parts = @()
    foreach ($key in $Body.Keys) {
        $jsonKey = ConvertTo-MiniDbJsonString -Value ([string]$key)
        $value = $Body[$key]

        if ($value -is [bool]) {
            $jsonValue = if ($value) { "true" } else { "false" }
        } else {
            $jsonValue = ConvertTo-MiniDbJsonString -Value ([string]$value)
        }

        $parts += ($jsonKey + ":" + $jsonValue)
    }

    return "{" + ($parts -join ",") + "}"
}

function Invoke-JsonPost {
    param(
        [string]$Url,
        [hashtable]$Body
    )

    $json = ConvertTo-MiniDbJson -Body $Body
    $tempFile = New-TemporaryFile
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)

    try {
        [System.IO.File]::WriteAllText($tempFile.FullName, $json, $utf8NoBom)

        $curlOutput = & curl.exe -sS `
            --http1.1 `
            --max-time 30 `
            -X POST $Url `
            -H "Content-Type: application/json" `
            --data-binary "@$($tempFile.FullName)" 2>&1
        $responseText = ($curlOutput | Out-String).Trim()

        if ($LASTEXITCODE -ne 0) {
            throw "curl.exe failed with exit code ${LASTEXITCODE}: $responseText"
        }

        return $responseText | ConvertFrom-Json
    } finally {
        Remove-Item -LiteralPath $tempFile.FullName -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-HealthCheck {
    param([string]$Url)

    $curlOutput = & curl.exe -sS --max-time 10 "$Url/health" 2>&1
    $responseText = ($curlOutput | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Health check failed. Is the API server running at ${Url}? $responseText"
    }

    return $responseText | ConvertFrom-Json
}

function Invoke-ApiQuery {
    param(
        [string]$Url,
        [string]$Sql,
        [bool]$IncludeProfile = $false,
        [string]$RequestId = ""
    )

    $body = @{
        sql = $Sql
    }

    if ($RequestId) {
        $body.request_id = $RequestId
    }

    if ($IncludeProfile) {
        $body.include_profile = $true
    }

    Invoke-JsonPost -Url "$Url/query" -Body $body
}

Write-Log "[1/4] Checking server health at $BaseUrl"
$health = Invoke-HealthCheck -Url $BaseUrl
if (-not $health.ok) {
    throw "Health check failed."
}

$baseId = [int]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() % 1000000000)

Write-Log "[2/4] Seeding $ConcurrentRequests row(s)"
for ($i = 0; $i -lt $ConcurrentRequests; $i++) {
    $id = $baseId + $i
    $insertSql = "INSERT INTO users VALUES ($id, 'tpuser$id', 25, 'tpuser$id@example.com');"
    $insertResult = Invoke-ApiQuery -Url $BaseUrl -Sql $insertSql -RequestId "seed-$id"
    if (-not $insertResult.ok) {
        throw "Seed insert failed for id=${id}: $($insertResult.error)"
    }
}

Write-Log "[3/4] Sending $ConcurrentRequests concurrent SELECT request(s)"
$jobs = @()
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

for ($i = 0; $i -lt $ConcurrentRequests; $i++) {
    $id = $baseId + $i
    $jobs += Start-Job -ArgumentList $BaseUrl, $id -ScriptBlock {
        param($Url, $UserId)

        try {
            function ConvertTo-MiniDbJsonString {
                param([string]$Value)

                $escaped = $Value.Replace('\', '\\')
                $escaped = $escaped.Replace('"', '\"')
                $escaped = $escaped.Replace("`r", '\r')
                $escaped = $escaped.Replace("`n", '\n')
                $escaped = $escaped.Replace("`t", '\t')

                return '"' + $escaped + '"'
            }

            function ConvertTo-MiniDbJson {
                param([hashtable]$Body)

                $parts = @()
                foreach ($key in $Body.Keys) {
                    $jsonKey = ConvertTo-MiniDbJsonString -Value ([string]$key)
                    $value = $Body[$key]

                    if ($value -is [bool]) {
                        $jsonValue = if ($value) { "true" } else { "false" }
                    } else {
                        $jsonValue = ConvertTo-MiniDbJsonString -Value ([string]$value)
                    }

                    $parts += ($jsonKey + ":" + $jsonValue)
                }

                return "{" + ($parts -join ",") + "}"
            }

            function Invoke-JsonPost {
                param(
                    [string]$Url,
                    [hashtable]$Body
                )

                $json = ConvertTo-MiniDbJson -Body $Body
                $tempFile = New-TemporaryFile
                $utf8NoBom = New-Object System.Text.UTF8Encoding($false)

                try {
                    [System.IO.File]::WriteAllText($tempFile.FullName, $json, $utf8NoBom)

                    $curlOutput = & curl.exe -sS `
                        --http1.1 `
                        --max-time 30 `
                        -X POST $Url `
                        -H "Content-Type: application/json" `
                        --data-binary "@$($tempFile.FullName)" 2>&1
                    $responseText = ($curlOutput | Out-String).Trim()

                    if ($LASTEXITCODE -ne 0) {
                        throw "curl.exe failed with exit code ${LASTEXITCODE}: $responseText"
                    }

                    return $responseText | ConvertFrom-Json
                } finally {
                    Remove-Item -LiteralPath $tempFile.FullName -Force -ErrorAction SilentlyContinue
                }
            }

            $bodyObject = @{
                sql = "SELECT * FROM users WHERE id = $UserId;"
                include_profile = $true
                request_id = "select-$UserId"
            }
            $response = Invoke-JsonPost -Url "$Url/query" -Body $bodyObject

            [pscustomobject]@{
                id = $UserId
                request_id = [string]$response.request_id
                ok = [bool]$response.ok
                row_count = [int]$response.row_count
                access_path = if ($response.profile) { [string]$response.profile.access_path } else { "" }
                error = [string]$response.error
            }
        } catch {
            [pscustomobject]@{
                id = $UserId
                ok = $false
                row_count = 0
                access_path = ""
                error = $_.Exception.Message
            }
        }
    }
}

$null = Wait-Job -Job $jobs
$results = $jobs | Receive-Job
$stopwatch.Stop()
$jobs | Remove-Job

if (-not $Quiet) {
    Write-Log "[4/4] Results"
    $results | Sort-Object id | Format-Table -AutoSize
}

$successCount = ($results | Where-Object {
    $_.ok -and $_.row_count -eq 1
}).Count
$failureCount = $ConcurrentRequests - $successCount
$firstError = ($results |
    Where-Object { -not ($_.ok -and $_.row_count -eq 1) } |
    Select-Object -First 1 -ExpandProperty error)
if (-not $firstError) {
    $firstError = ""
}

$elapsedMs = $stopwatch.Elapsed.TotalMilliseconds

Write-Log ""
Write-Log "Successful responses: $successCount / $ConcurrentRequests"
Write-Log ("Elapsed wall time: {0:N0} ms" -f $elapsedMs)

if ($successCount -ne $ConcurrentRequests -and -not $PassThru) {
    throw "Thread pool smoke test failed."
}

if ($successCount -eq $ConcurrentRequests) {
    Write-Log "Thread pool smoke test passed."
} else {
    Write-Log "Thread pool smoke test failed."
}

if ($PassThru) {
    [pscustomobject]@{
        concurrent_requests = $ConcurrentRequests
        success_count = $successCount
        failure_count = $failureCount
        elapsed_ms = [math]::Round($elapsedMs, 0)
        requests_per_second = if ($elapsedMs -gt 0) {
            [math]::Round($ConcurrentRequests / ($elapsedMs / 1000.0), 2)
        } else {
            0
        }
        ok_requests_per_second = if ($elapsedMs -gt 0) {
            [math]::Round($successCount / ($elapsedMs / 1000.0), 2)
        } else {
            0
        }
        first_error = $firstError
        passed = ($successCount -eq $ConcurrentRequests)
    }
}
