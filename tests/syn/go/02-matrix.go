//go:build linux
// +build amd64
// ordinary comment
package main

/* block comment */
const answer = 42
const tag = `json:"name"\nraw`
var text = "line\n\u00e9\q"
var runeValue = '\U0001F642'
var badRune = '\q'
var flags = true || false
var absent = nil
var nums = []any{0xff_ff, 0b1010, 0o755, 1_000.5e-2i, .5i}

func main() {
start:
	println(append(nums, iota), text, tag, runeValue)
	if answer >= len(nums) { goto start }
}

// Sprint 42 deterministic variant: go-02
