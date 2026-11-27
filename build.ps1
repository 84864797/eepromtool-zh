[CmdletBinding()]
param(
    [string]$Zig = 'zig'
)

$ErrorActionPreference = 'Stop'
$archive = Join-Path $PSScriptRoot 'soem-v2.0.1.zip'
$expectedHash = 'E884F26A5B45EB522C57A0A9BDF6655AE61C6BE7287BBAF44DA5946FEE696913'
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'Unexpected SOEM v2.0.1 source archive.'
}

$work = Join-Path ([IO.Path]::GetTempPath()) "eepromtool-zh-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
& tar.exe -xf $archive -C $work
if ($LASTEXITCODE -ne 0) { throw 'Could not extract SOEM.' }
$upstream = Join-Path $work 'SOEM-2.0.1'

# Generate the header with the defaults declared by the upstream CMake project.
$values = @{}
$cmake = Get-Content -LiteralPath (Join-Path $upstream 'CMakeLists.txt') -Raw
$pattern = '(?m)^set\((EC_[A-Z0-9_]+)\s+(?:"([^"]+)"|([^\s"]+))\s+CACHE STRING'
foreach ($setting in [regex]::Matches($cmake, $pattern)) {
    $key = $setting.Groups[1].Value
    if ($setting.Groups[2].Success) {
        $values[$key] = $setting.Groups[2].Value
    } else {
        $values[$key] = $setting.Groups[3].Value
    }
}
foreach ($kind in @('PRIMARY', 'SECONDARY')) {
    $octets = $values["EC_${kind}_MAC"] -split ':'
    $words = for ($i = 0; $i -lt $octets.Length; $i += 2) {
        "0x$($octets[$i])$($octets[$i + 1])"
    }
    $values["EC_${kind}_MAC_ARRAY"] = '{' + ($words -join ', ') + '}'
}
$template = Join-Path $upstream 'include/soem/ec_options.h.in'
$header = [regex]::Replace((Get-Content -LiteralPath $template -Raw), '@(EC_[A-Z0-9_]+)@', {
    param($match)
    $key = $match.Groups[1].Value
    if (!$values.ContainsKey($key)) { throw "Missing SOEM option $key." }
    $values[$key]
})
Set-Content -LiteralPath (Join-Path $upstream 'include/soem/ec_options.h') -Value $header -Encoding ascii

$libraries = Join-Path $upstream 'oshw/win32/wpcap/Lib/x64'
$sourceFiles = (Get-ChildItem -LiteralPath (Join-Path $upstream 'src') -Filter '*.c' -File).FullName
$output = Join-Path $PSScriptRoot 'eepromtool.exe'
$compile = @(
    'cc', '-target', 'x86_64-windows-gnu', '-O2', '-std=gnu11', '-D_UCRT',
    '-I', (Join-Path $upstream 'include'),
    '-I', (Join-Path $upstream 'osal'),
    '-I', (Join-Path $upstream 'osal/win32'),
    '-I', (Join-Path $upstream 'oshw/win32'),
    '-I', (Join-Path $upstream 'oshw/win32/wpcap/Include'),
    (Join-Path $PSScriptRoot 'eepromtool.c'),
    (Join-Path $upstream 'osal/win32/osal.c'),
    (Join-Path $upstream 'oshw/win32/oshw.c'),
    (Join-Path $upstream 'oshw/win32/nicdrv.c')
) + $sourceFiles + @(
    (Join-Path $libraries 'wpcap.lib'),
    (Join-Path $libraries 'Packet.lib'),
    '-lws2_32', '-lwinmm', '-lucrtbase', '-s', '-o', $output
)
& $Zig @compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Get-Item -LiteralPath $output
