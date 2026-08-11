# Native language pack

```wolf
fn answer() -> i32 { return 42 }
```

```cpp
template<typename T> T answer(T value) { return value; }
```

```objective-c
@interface Answer : NSObject
```

```java
record Answer(int value) {}
```

```kotlin
val answer = "value=${42}"
```

```csharp
var answer = $"value={42}";
```

```swift
let answer = #"value=\#(42)"#
```

```zig
const answer: u32 = 42;
```

```lua
local answer = [=[forty-two]=]
```

```ruby
answer = "value=#{42}"
```

```perl
my $answer = q{forty-two};
```

```r
answer <- `forty two`
```

```julia
answer = """forty-two"""
```

```dart
final answer = 'value=${42}';
```

```powershell
$answer = "value=$($value)"
```

```zsh
answer=${value:-42}
```

```fish
set answer (printf 42)
```

```sql
SELECT "answer" FROM results;
```

```nix
answer = ''value=${value}'';
```

```haskell
answer :: Integer
```

```ocaml
let answer = Some 42
```

```xml
<answer value="42"><![CDATA[forty-two]]></answer>
```

```graphql
query Answer { answer(value: $value) }
```

```protobuf
optional uint32 answer = 42;
```

```hcl
answer = "value=${var.value}"
```

```dockerfile
RUN printf '%s\n' "$ANSWER"
```

```cmake
set(ANSWER "${VALUE}")
```

```meson
answer = dependency('answer', required: true)
```

```diff
@@ -1 +1 @@
```
