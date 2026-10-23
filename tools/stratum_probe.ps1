# stratum_probe.ps1 - connects to a stratum port (optionally TLS), subscribes/authorizes
# and logs the raw lines received for a few seconds (no mining, no share).
#   -Mode std   : subscribe [agent] then authorize, without waiting
#   -Mode wait  : waits for the subscribe reply before authorizing
#   -Mode none  : sends nothing (checks whether the server closes by itself)
#   -Mode nullp : subscribe [agent, null]
param([string]$HostName = "stratum-eu.rplant.xyz", [int]$Port = 7041,
      [string]$User = "YOUR_KRGN_ADDRESS.probe", [int]$Seconds = 20,
      [string]$Agent = "krgnminer/0.1.0", [string]$Log = "", [string]$Mode = "std", [switch]$Tls)
$ErrorActionPreference = "Stop"
function W($s) { $l = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $s; Write-Output $l; if ($Log) { Add-Content -Path $Log -Value $l } }
try {
    W "connect ${HostName}:$Port tls=$Tls mode=$Mode agent=$Agent"
    $c = New-Object System.Net.Sockets.TcpClient
    $c.ReceiveTimeout = 1000
    $c.Connect($HostName, $Port)
    $s = $c.GetStream()
    if ($Tls) {
        $ssl = New-Object System.Net.Security.SslStream($s, $false, ({ $true } -as [System.Net.Security.RemoteCertificateValidationCallback]))
        $ssl.AuthenticateAsClient($HostName)
        W ("tls ok " + $ssl.SslProtocol)
        $s = $ssl
    }
    $wr = New-Object System.IO.StreamWriter($s); $wr.NewLine = "`n"; $wr.AutoFlush = $true
    $rd = New-Object System.IO.StreamReader($s)
    $m1 = '{"id":1,"method":"mining.subscribe","params":["' + $Agent + '"]}'
    if ($Mode -eq "nullp") { $m1 = '{"id":1,"method":"mining.subscribe","params":["' + $Agent + '",null]}' }
    $m2 = '{"id":2,"method":"mining.authorize","params":["' + $User + '","x"]}'
    if ($Mode -ne "none") { W "-> $m1"; $wr.WriteLine($m1) }
    if ($Mode -eq "std" -or $Mode -eq "nullp") { W "-> $m2"; $wr.WriteLine($m2) }
    $sentAuth = ($Mode -ne "wait")
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        try { $line = $rd.ReadLine() } catch { $line = $null; continue }
        if ($line -eq $null) { W "connection closed"; break }
        W "<- $line"
        if (-not $sentAuth) { W "-> $m2"; $wr.WriteLine($m2); $sentAuth = $true }
    }
    $c.Close()
} catch { W ("ERROR " + $_.Exception.Message) }
