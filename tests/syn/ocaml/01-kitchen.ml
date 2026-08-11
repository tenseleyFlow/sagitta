open Demo
module Parser = Core.Parser
type choice = Yes | No
let rec map ~name ?limit xs =
  match xs with
  | [] -> `Empty
  | x :: rest when true -> (x + 0x10, map ~name ?limit rest)
let ratio = 1.25e-2
