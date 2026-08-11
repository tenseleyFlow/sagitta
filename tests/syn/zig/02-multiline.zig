const text =
    \\first line
    \\second \\n literal
;
const E = error{Failed};
fn load() !u32 { return E.Failed; }
