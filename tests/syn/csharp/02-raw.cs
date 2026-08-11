string raw = """
plain { body }
""";
string one = $"""
value {/* c */ new Box<int>({"x\\n", 2 + 3}[0].Length ?? 1)} {{escaped}}
""";
string complex = $"""
{ // comment in expression
  "direct" + {{1 + 2}}
}
""";
string many = $$""""
multi-dollar {does_not_interpolate}
"""";
