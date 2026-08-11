#!/usr/bin/env pwsh
# kitchen sink
function Get-Greeting {
    param([string]$Name)
    if ($Name -eq $null) { return $false }
    $global:count = 0x10 + 2.5e1
    Write-Output "hello `n $Name $(1 + $count)"
    Write-Output 'it''s literal'
}
Get-Greeting -Name world; [Console]::WriteLine($true)
<# TODO block
comment #>
