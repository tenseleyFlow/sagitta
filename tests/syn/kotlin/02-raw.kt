val page = """
    hello $name
    value=${/* c */ map[0]?.get({"k\n" to 1 + 2}) ?: 1}
"""
val complex = """
${ // comment in expression
    {{"nested": 1}}
}
"""
