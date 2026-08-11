namespace Demo;
public interface IService<in T, out R> {
  R Map(T value);
}
public static class Entry {
  public static int Main(string[] args) => args?.Length ?? 0;
}
