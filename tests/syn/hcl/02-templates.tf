locals {
  message = "hello $${literal} ${format("%s", user.name)} %{ if ready }go%{ else }wait%{ endif } %%"
  computed = "${true + 2 != "x\\n"}"
  escaped = "line\\nnext"
  broken = "unterminated ${value
}
