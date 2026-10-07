# =============================================================================
# apply_overlay.ps1 —— 把 AC / 搭路(Telly) / 蹲起塔 三个模块 + 输入注入层
#                       叠加到上游 InfiniteGUI-Minecraft-DLL 仓库上
#
# 用法：
#   pwsh -File tools/apply_overlay.ps1 -RepoRoot <上游仓库根目录>
#   pwsh -File tools/apply_overlay.ps1 -RepoRoot upstream -WhatIf
#
# 特点：
#   * 只**新增**文件，绝不覆盖上游已有文件（脚本会先校验文件名不冲突）
#   * 对 ItemManager.cpp / .vcxproj 做幂等插入（跑几次结果都一样）
#   * 上游源码是 GBK 编码，这里用 Latin-1 逐字节往返读写，保证原文一个字节都不变
#   * 新文件统一补 UTF-8 BOM：MSVC 靠 BOM 判定 UTF-8，u8"中文" 才不会变乱码
# =============================================================================

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [string]$OverlayDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'overlay\InfiniteGUI-DLL'),

    # 目标文件名（新增，不覆盖）
    [string[]]$Files = @(
        'AcInputLink.h',
        'InputSim.h', 'InputSim.cpp',
        'AcClicker.h', 'AcClicker.cpp',
        'Telly.h', 'Telly.cpp',
        'SneakTower.h', 'SneakTower.cpp'
    )
)

$ErrorActionPreference = 'Stop'

# 逐字节往返用的编码（每个字节 <-> 一个字符，不丢任何字节）
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Read-RawText([string]$Path)
{
    return $Latin1.GetString([System.IO.File]::ReadAllBytes($Path))
}

function Write-RawText([string]$Path, [string]$Text)
{
    [System.IO.File]::WriteAllBytes($Path, $Latin1.GetBytes($Text))
}

function Add-Utf8Bom([string]$Path)
{
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    {
        return $false
    }
    $bom = [byte[]](0xEF, 0xBB, 0xBF)
    $out = New-Object byte[] ($bytes.Length + 3)
    [Array]::Copy($bom, 0, $out, 0, 3)
    [Array]::Copy($bytes, 0, $out, 3, $bytes.Length)
    [System.IO.File]::WriteAllBytes($Path, $out)
    return $true
}

# ------------------------------------------------------------------ 0. 校验

$projDir = Join-Path $RepoRoot 'InfiniteGUI-DLL'
$vcxproj = Join-Path $projDir 'InfiniteGUI-DLL.vcxproj'
$itemMgr = Join-Path $projDir 'ItemManager.cpp'

foreach ($p in @($vcxproj, $itemMgr))
{
    if (-not (Test-Path -LiteralPath $p))
    {
        throw "找不到上游文件：$p（-RepoRoot 要指向含 InfiniteGUI-DLL.sln 的仓库根目录）"
    }
}

if (-not (Test-Path -LiteralPath $OverlayDir))
{
    throw "找不到 overlay 目录：$OverlayDir"
}

Write-Host "[overlay] 上游仓库 : $RepoRoot"
Write-Host "[overlay] overlay 源 : $OverlayDir"

# ------------------------------------------------------------------ 1. 新增源文件

$copied = 0
foreach ($name in $Files)
{
    $src = Join-Path $OverlayDir $name
    $dst = Join-Path $projDir $name

    if (-not (Test-Path -LiteralPath $src)) { throw "overlay 缺文件：$src" }

    if (Test-Path -LiteralPath $dst)
    {
        # 已经叠加过：允许更新（这就是我们的文件），但先记录
        Write-Host "  [更新] $name"
    }
    else
    {
        Write-Host "  [新增] $name"
    }

    if ($PSCmdlet.ShouldProcess($dst, '复制'))
    {
        Copy-Item -LiteralPath $src -Destination $dst -Force
        if (Add-Utf8Bom $dst) { Write-Host "         + UTF-8 BOM" }
        $copied++
    }
}

# ------------------------------------------------------------------ 2. 改 ItemManager.cpp

$text = Read-RawText $itemMgr
$nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$changed = $false

# 2.1 include
if ($text -notmatch 'AcClicker\.h')
{
    $anchor = '#include "AutoText.h"'
    if ($text -notmatch [regex]::Escape($anchor))
    {
        throw "ItemManager.cpp 里找不到 include 锚点：$anchor"
    }
    $inject = @(
        '',
        '// === overlay: AC / Telly / SneakTower ===',
        '#include "InputSim.h"',
        '#include "AcClicker.h"',
        '#include "Telly.h"',
        '#include "SneakTower.h"'
    ) -join $nl
    $text = $text.Replace($anchor, $anchor + $nl + $inject)
    $changed = $true
    Write-Host '  [补丁] ItemManager.cpp: include 已插入'
}
else
{
    Write-Host '  [跳过] ItemManager.cpp: include 已存在'
}

# 2.2 AddItem 注册
if ($text -notmatch 'AcClicker::Instance')
{
    $anchor = 'AddItem(&NotificationItem::Instance());'
    if ($text -notmatch [regex]::Escape($anchor))
    {
        throw "ItemManager.cpp 里找不到注册锚点：$anchor"
    }
    $inject = @(
        '',
        '    // === overlay: AC / Telly / SneakTower ===',
        '    AddItem(&AcClicker::Instance());',
        '    AddItem(&Telly::Instance());',
        '    AddItem(&SneakTower::Instance());'
    ) -join $nl
    $text = $text.Replace($anchor, $anchor + $nl + $inject)
    $changed = $true
    Write-Host '  [补丁] ItemManager.cpp: 模块注册已插入'
}
else
{
    Write-Host '  [跳过] ItemManager.cpp: 模块注册已存在'
}

if ($changed -and $PSCmdlet.ShouldProcess($itemMgr, '写回'))
{
    Write-RawText $itemMgr $text
}

# ------------------------------------------------------------------ 3. 改 vcxproj

$xml = Read-RawText $vcxproj
$xmlNl = if ($xml.Contains("`r`n")) { "`r`n" } else { "`n" }
$xmlChanged = $false

$compileAdd = @('AcClicker.cpp', 'InputSim.cpp', 'SneakTower.cpp', 'Telly.cpp')
$includeAdd = @('AcClicker.h', 'AcInputLink.h', 'InputSim.h', 'SneakTower.h', 'Telly.h')

if ($xml -notmatch 'AcClicker\.cpp')
{
    $anchor = '<ClCompile Include="Sprint.cpp" />'
    if ($xml -notmatch [regex]::Escape($anchor))
    {
        throw "vcxproj 里找不到 ClCompile 锚点：$anchor"
    }
    $inject = ($compileAdd | ForEach-Object { "    <ClCompile Include=`"$_`" />" }) -join $xmlNl
    $xml = $xml.Replace($anchor, $anchor + $xmlNl + $inject)
    $xmlChanged = $true
    Write-Host '  [补丁] vcxproj: ClCompile 条目已插入'
}
else
{
    Write-Host '  [跳过] vcxproj: ClCompile 条目已存在'
}

if ($xml -notmatch 'AcClicker\.h')
{
    $anchor = '<ClInclude Include="Sprint.h" />'
    if ($xml -notmatch [regex]::Escape($anchor))
    {
        throw "vcxproj 里找不到 ClInclude 锚点：$anchor"
    }
    $inject = ($includeAdd | ForEach-Object { "    <ClInclude Include=`"$_`" />" }) -join $xmlNl
    $xml = $xml.Replace($anchor, $anchor + $xmlNl + $inject)
    $xmlChanged = $true
    Write-Host '  [补丁] vcxproj: ClInclude 条目已插入'
}
else
{
    Write-Host '  [跳过] vcxproj: ClInclude 条目已存在'
}

if ($xmlChanged -and $PSCmdlet.ShouldProcess($vcxproj, '写回'))
{
    Write-RawText $vcxproj $xml
}

Write-Host "[overlay] 完成：复制 $copied 个文件，ItemManager/vcxproj 已就绪。"