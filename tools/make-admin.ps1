# SPDX-License-Identifier: MIT
# Gera o GNClicker-Admin.exe: copia o exe normal e troca SOMENTE o manifesto
# (RT_MANIFEST, id 1) pelo gnclicker.manifest, que pede privilegios de administrador.
# O codigo e os demais recursos (icone, metadados de versao) continuam identicos.
#
# Por que nao compilar direto com o manifesto? Os runtimes MinGW recentes ja embutem um
# manifesto padrao, e o linker recusa um segundo ("multiple non-default manifests").
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Dest,
    [Parameter(Mandatory = $true)][string]$Manifest
)
$ErrorActionPreference = "Stop"

Add-Type -Namespace GN -Name Res -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr BeginUpdateResourceW(string file, bool deleteExisting);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool UpdateResourceW(System.IntPtr h, System.IntPtr type, System.IntPtr name, ushort lang, byte[] data, uint size);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool EndUpdateResourceW(System.IntPtr h, bool discard);
'@

$src = (Resolve-Path $Source).Path
$man = (Resolve-Path $Manifest).Path
$dst = Join-Path (Split-Path $src -Parent) (Split-Path $Dest -Leaf)
Copy-Item $src $dst -Force

$data = [System.IO.File]::ReadAllBytes($man)
$h = [GN.Res]::BeginUpdateResourceW($dst, $false)
if ($h -eq [IntPtr]::Zero) { throw "BeginUpdateResource falhou para $dst" }
# 24 = RT_MANIFEST, 1 = CREATEPROCESS_MANIFEST_RESOURCE_ID, 1033 = en-US (mesmo idioma do manifesto padrao)
$ok = [GN.Res]::UpdateResourceW($h, [IntPtr]24, [IntPtr]1, 1033, $data, [uint32]$data.Length)
if (-not $ok) { [GN.Res]::EndUpdateResourceW($h, $true) | Out-Null; throw "UpdateResource falhou" }
if (-not [GN.Res]::EndUpdateResourceW($h, $false)) { throw "EndUpdateResource falhou" }
Write-Host "[OK] $dst"
