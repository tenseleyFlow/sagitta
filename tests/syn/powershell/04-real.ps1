enum State { Ready; Done }
try {
    foreach ($item in 1..3) {
        Write-Output -Verbose $item
    }
} catch {
    throw $_
} finally {
    exit 0
}
