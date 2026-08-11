module type STORE = sig
  type t
  val create : name:string -> ?size:int -> t
  exception Missing of string
end
