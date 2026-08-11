const std = @import("std");
/// docs
//! module docs
// ordinary
pub const Kind = enum { one, two };
const Pair = packed struct { left: u32, right: u32 };
var mask: u64 = 0xCA_FE + 0b1010;
const ratio: f64 = 1.5e2;
const ready: bool = true;
const missing = null;
const unknown = undefined;
test "run" { if (ready and mask >= 1) try call('\\n', "x\\u{41}"); else unreachable; }
