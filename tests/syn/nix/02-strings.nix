let who = "world"; in {
  normal = "hello \\n ${if { nested = 7; } then "yes" else who + 1 # expression comment
}";
  indented = ''
    literal '''' and escaped ''$ marker
    answer ${let x = 2; in x * 3}
  '';
}
