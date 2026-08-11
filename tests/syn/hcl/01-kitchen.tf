# Terraform-shaped kitchen sink.
terraform { required_version = ">= 1.6" }
resource "demo_widget" "main" {
  enabled = true
  empty = null
  count = 0x2a + 3.5e-2
  choice = flag ? "yes" : "no"
  rule = for item in values : item if item != 0
  tags = { owner = "ops" }
}
/* TODO: keep the provider pin
   FIXME before release */
