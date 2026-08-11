# kitchen
`odd name` <- function(x) {
  if (x %in% c(TRUE, FALSE)) return(0x10 + 1.5e2L)
  else for (i in 1:3) x <<- x %% i
  while (!is.na(x) && x >= 0) break
  repeat { next }
  NULL; NA_real_; Inf; NaN
}
