//! crate docs
/// item docs
// ordinary
/* outer /* inner TODO */ done */
#[outer[nested]]
pub async fn example<'a>(x: &'a str) -> i32 {
    let raw = r###"raw \n "## still"###;
    let bytes = b"byte\x41\q";
    let text = "text\u{1f642}\q";
    let chars = ('a', b'\x41', '\n', 'static, '_, 'life, ');
    let ids = (r#type, MACRO_CONST, CamelCase, true);
    println!("{}", ids);
    let nums = (0xffu8, 0o77i16, 0b1010usize, 1_000.5e-2f64);
    if x.is_empty() { return nums.3 as i32; }
    0
}
/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/* depth */*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/
