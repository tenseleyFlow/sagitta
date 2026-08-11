package demo
/// docs
// ordinary line comment
/* outer /* nested TODO */ done */
@JvmInline value class UserId(val raw: Long)
sealed interface Result
data class User(val name: String, val age: Int)
fun greet(user: User?): String {
    val n = 0xCA_FEuL + 1.5e2F
    return if (user?.name != null) "hello $user ${user.name ?: "none"}" else "missing\\n"
}
val char = '\n'
val truth = true
val absent = null
*/
