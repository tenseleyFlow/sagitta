import Foundation
/// docs
// ordinary line comment
/* outer /* nested TODO */ done */
@available(macOS 14, *)
public final class Box<T> {
    let value: T
    weak var owner: Any?
    init(_ value: T) { self.value = value }
    func render(_ n: Int) -> String {
        let hex = 0xCA_FE + 1.5e2
        if n >= 0 && true { return "value \(value) nested \((n + 1))\n" }
        return String(describing: nil)
    }
}
*/
