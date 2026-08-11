#= outer
   #= nested =#
=#
s = """triple $name $(call((x)))
still \q
"""
c2 = "plain $name $(call((x))) \q"
c = `echo $name $(value) \q`
ch = '\n'
