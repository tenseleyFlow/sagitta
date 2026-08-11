package demo;
import java.util.List;
/// line docs
// ordinary line comment
/** @param value docs */
/* TODO block */
@Deprecated
public sealed class Box<T extends Number> permits TinyBox {
  private static final long MASK = 0xCA_FEL;
  double ratio = 1.25e-3D;
  boolean ready = true;
  Object absent = null;
  char newline = '\n';
  String escaped = "x\u0041\q";
  int run(int value) { if (value >= 0) return value++; else throw new IllegalArgumentException(); }
}
*/
