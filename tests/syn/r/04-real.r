# .Rprofile-shaped startup code
options(stringsAsFactors = FALSE)
if (interactive()) {
  .First <- function() message("ready")
}
