object App {
    suspend fun run(args: Array<String>) {
        for (arg in args) when {
            arg.isEmpty() -> continue
            else -> println(arg)
        }
    }
}
