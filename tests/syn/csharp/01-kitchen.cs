using System;
/// docs
// ordinary line comment
/* TODO block */
[Obsolete]
public sealed record Box<T>(T Value) {
  const ulong Mask = 0xCA_FEUL;
  decimal Rate = 1.25e2M;
  bool Ready = true;
  object Missing = null;
  char Newline = '\n';
  string Basic = "x\\u0041";
  string Verbatim = @"a ""quote""";
  string Interp = $"value={{literal}} {Value ?? default} escape \\n";
  string Both = $@"path {{literal}} {Value} ""ok""";
  public async System.Threading.Tasks.Task<int> Run(int x) { if (x >= 0) return await Next(x); throw new Exception(); }
}
*/
