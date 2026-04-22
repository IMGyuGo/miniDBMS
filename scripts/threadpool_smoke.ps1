param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [int]$ConcurrentRequests = 8
)

$ErrorActionPreference = "Stop"

function Invoke-ApiQuery {
    param(
        [string]$Url,
        [string]$Sql,
        [bool]$IncludeProfile = $false
    )

    $body = @{
        sql = $Sql
    }

    if ($IncludeProfile) {
        $body.include_profile = $true
    }

    Invoke-RestMethod -Uri "$Url/query" `
                      -Method Post `
                      -ContentType "application/json" `
                      -Body ($body | ConvertTo-Json -Compress)
}

Write-Host "[1/4] Checking server health at $BaseUrl"
$health = Invoke-RestMethod -Uri "$BaseUrl/health" -Method Get
if (-not $health.ok) {
    throw "Health check failed."
}

$baseId = [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 1000000000)

Write-Host "[2/4] Seeding $ConcurrentRequests row(s)"
for ($i = 0; $i -lt $ConcurrentRequests; $i++) {
    $id = $baseId + $i
    $insertSql = "INSERT INTO users VALUES ($id, 'tpuser$id', 25, 'tpuser$id@example.com');"
    $insertResult = Invoke-ApiQuery -Url $BaseUrl -Sql $insertSql
    if (-not $insertResult.ok) {
        throw "Seed insert failed for id=$id: $($insertResult.error)"
    }
}

Write-Host "[3/4] Sending $ConcurrentRequests concurrent SELECT request(s)"
$jobs = @()
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

for ($i = 0; $i -lt $ConcurrentRequests; $i++) {
    $id = $baseId + $i
    $jobs += Start-Job -ArgumentList $BaseUrl, $id -ScriptBlock {
        param($Url, $UserId)

        try {
            $body = @{
                sql = "SELECT * FROM users WHERE id = $UserId;"
                include_profile = $true
            } | ConvertTo-Json -Compress

            $response = Invoke-RestMethod -Uri "$Url/query" `
                                          -Method Post `
                                          -ContentType "application/json" `
                                          -Body $body

            [pscustomobject]@{
                id = $UserId
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

Write-Host "[4/4] Results"
$results | Sort-Object id | Format-Table -AutoSize

$successCount = ($results | Where-Object {
    $_.ok -and $_.row_count -eq 1
}).Count

Write-Host ""
Write-Host "Successful responses: $successCount / $ConcurrentRequests"
Write-Host ("Elapsed wall time: {0:N0} ms" -f $stopwatch.Elapsed.TotalMilliseconds)

if ($successCount -ne $ConcurrentRequests) {
    throw "Thread pool smoke test failed."
}

Write-Host "Thread pool smoke test passed."
