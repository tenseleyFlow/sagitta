var a = "hello $name ${map[{{key: 1}: 2}]} \\n";
var b = 'single $name ${call()} \q';
var c = r"raw $name \\n";
var d = r'raw ${name}';
/* outer /* nested */ done */
