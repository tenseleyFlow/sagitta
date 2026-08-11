let raw = #"raw \#(not modeled)"#
let tagged = ###"multi
line "## still
close"###
let rawBlock = ##"""multi
line
"""##
let block = """
ordinary \(/* c */ ["x\n", 2 + 3][0])
escape \n
\( // comment in expression
    (("inner") + 1)
)
"""
