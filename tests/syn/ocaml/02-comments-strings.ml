(* outer (* nested *) comment *)
let ordinary = "escaped \\n and quote \""
let char = '\t'
let quoted = {tag|multiline
text with "quotes" and (* markers *)
|tag}
let empty = {|plain quoted text|}
