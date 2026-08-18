param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$TargetIp
)

$targetIp = $TargetIp
$allTestsPassed = $true

# --- Test 1 ---
Write-Host "--- Test 1: GET http://$targetIp/api/telemetry ---"
try {
    $response = Invoke-WebRequest -Uri "http://$targetIp/api/telemetry" -TimeoutSec 15 -ErrorAction Stop
    Write-Host "Status Code: $($response.StatusCode)"
    Write-Host "Content-Type: $($response.Headers['Content-Type'])"
    Write-Host "Body: $($response.Content)"
    
    # Validations
    $valid1 = $true
    if ($response.StatusCode -ne 200) { Write-Host "[FAIL] Status is not 200"; $valid1 = $false }
    if ($response.Headers['Content-Type'] -notlike "*application/json*") { Write-Host "[FAIL] Content-Type does not contain application/json"; $valid1 = $false }
    
    try {
        $json = $response.Content | ConvertFrom-Json
        if ($json.deviceId -notlike "az3166-*") { Write-Host "[FAIL] deviceId '$($json.deviceId)' does not start with az3166-"; $valid1 = $false }
        
        # Check numeric temp/humidity/pressure
        if ($null -eq $json.temperature) { Write-Host "[FAIL] temperature is null"; $valid1 = $false }
        else {
            [double]$temp = 0
            if (-not [double]::TryParse($json.temperature.ToString(), [ref]$temp)) { Write-Host "[FAIL] temperature is not numeric"; $valid1 = $false }
        }
        if ($null -eq $json.humidity) { Write-Host "[FAIL] humidity is null"; $valid1 = $false }
        else {
            [double]$hum = 0
            if (-not [double]::TryParse($json.humidity.ToString(), [ref]$hum)) { Write-Host "[FAIL] humidity is not numeric"; $valid1 = $false }
        }
        if ($null -eq $json.pressure) { Write-Host "[FAIL] pressure is null"; $valid1 = $false }
        else {
            [double]$press = 0
            if (-not [double]::TryParse($json.pressure.ToString(), [ref]$press)) { Write-Host "[FAIL] pressure is not numeric"; $valid1 = $false }
        }
    } catch {
        Write-Host "[FAIL] Could not parse body as JSON: $_"
        $valid1 = $false
    }
    
    if ($valid1) {
        Write-Host "[PASS] Test 1 succeeded"
    } else {
        Write-Host "[FAIL] Test 1 failed validations"
        $allTestsPassed = $false
    }
} catch {
    Write-Host "[FAIL] Test 1 Error: $_"
    $allTestsPassed = $false
}

# --- Test 2 ---
Write-Host "`n--- Test 2: GET http://$targetIp/api/unknown ---"
$response2 = $null
$response2Error = $null
$requestParameters = @{
    Uri = "http://$targetIp/api/unknown"
    ErrorAction = "Stop"
}
if ((Get-Command Invoke-WebRequest).Parameters.ContainsKey("SkipHttpErrorCheck")) {
    $requestParameters.SkipHttpErrorCheck = $true
}
try {
    $response2 = Invoke-WebRequest @requestParameters
} catch {
    $response2Error = $_
    $response2 = $_.Exception.Response
}

if ($response2) {
    $statusCode2 = 0
    if ($response2 -is [System.Net.HttpWebResponse] -or $response2.GetType().Name -eq "HttpWebResponse") {
        $statusCode2 = [int]$response2.StatusCode
        $contentType2 = $response2.Headers["Content-Type"]
        $streamReader = New-Object System.IO.StreamReader($response2.GetResponseStream())
        $body2 = $streamReader.ReadToEnd()
        $streamReader.Close()
        $response2.Close()
    } elseif ($response2.GetType().FullName -eq "System.Net.Http.HttpResponseMessage") {
        $statusCode2 = [int]$response2.StatusCode
        $contentType2 = $response2.Content.Headers.ContentType.ToString()
        $body2 = $response2Error.ErrorDetails.Message
    } else {
        $statusCode2 = $response2.StatusCode
        $contentType2 = $response2.Headers["Content-Type"]
        $body2 = $response2.Content
    }
    
    Write-Host "Status Code: $statusCode2"
    Write-Host "Content-Type: $contentType2"
    Write-Host "Body: $body2"
    
    $valid2 = $true
    if ($statusCode2 -ne 404) { Write-Host "[FAIL] Status is not 404"; $valid2 = $false }
    if ($contentType2 -notlike "*application/json*") { Write-Host "[FAIL] Content-Type does not contain application/json"; $valid2 = $false }
    
    $expectedBody = '{"error":"not found"}'
    $trimmedBody = $body2.Trim()
    if ($trimmedBody -ne $expectedBody) { Write-Host "[FAIL] Body is not exact: Expected '$expectedBody', Got '$trimmedBody'"; $valid2 = $false }
    
    if ($valid2) {
        Write-Host "[PASS] Test 2 succeeded"
    } else {
        Write-Host "[FAIL] Test 2 failed validations"
        $allTestsPassed = $false
    }
} else {
    Write-Host "[FAIL] No response received for Test 2"
    $allTestsPassed = $false
}

# --- Test 3 ---
Write-Host "`n--- Test 3: Raw TCP connection to $targetIp:80 ---"
try {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 5000
    $client.SendTimeout = 5000
    $client.Connect($targetIp, 80)
    $stream = $client.GetStream()
    
    $sendBytes = [System.Text.Encoding]::ASCII.GetBytes("`r`n`r`n")
    $stream.Write($sendBytes, 0, $sendBytes.Length)
    
    $buffer = New-Object byte[] 4096
    $responseBytes = New-Object System.Collections.Generic.List[byte]
    $bytesRead = 0
    
    # Read until EOF or timeout
    do {
        try {
            $bytesRead = $stream.Read($buffer, 0, $buffer.Length)
            if ($bytesRead -gt 0) {
                # Add to responseBytes
                for ($i = 0; $i -lt $bytesRead; $i++) {
                    $responseBytes.Add($buffer[$i])
                }
            }
        } catch {
            Write-Host "Read timed out or connection reset: $_"
            $bytesRead = 0
        }
    } while ($bytesRead -gt 0)
    
    $rawResponse = [System.Text.Encoding]::ASCII.GetString($responseBytes.ToArray())
    Write-Host "Raw Response:`n$rawResponse"
    
    $valid3 = $true
    if ($rawResponse -notmatch "^HTTP/1.1 400 Bad Request") {
        Write-Host "[FAIL] Status line is not HTTP/1.1 400 Bad Request"; $valid3 = $false
    }
    if ($rawResponse -notmatch "Content-Type:\s*application/json") {
        Write-Host "[FAIL] Content-Type is not application/json"; $valid3 = $false
    }
    
    # Extract body - Usually body is after \r\n\r\n
    $splitIndex = $rawResponse.IndexOf("`r`n`r`n")
    if ($splitIndex -ge 0) {
        $body3 = $rawResponse.Substring($splitIndex + 4)
        $trimmedBody3 = $body3.Trim()
        $expectedBody3 = '{"error":"bad request"}'
        if ($trimmedBody3 -ne $expectedBody3) {
            Write-Host "[FAIL] Body is not exact: Expected '$expectedBody3', Got '$trimmedBody3'"; $valid3 = $false
        }
    } else {
        Write-Host "[FAIL] Response headers and body not separated by \r\n\r\n"; $valid3 = $false
    }
    
    if ($valid3) {
        Write-Host "[PASS] Test 3 succeeded"
    } else {
        Write-Host "[FAIL] Test 3 failed validations"
        $allTestsPassed = $false
    }
    
    $stream.Close()
    $client.Close()
} catch {
    Write-Host "[FAIL] Test 3 Error: $_"
    $allTestsPassed = $false
}

if (-not $allTestsPassed) {
    exit 1
}
