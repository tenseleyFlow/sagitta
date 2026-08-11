@FunctionalInterface
interface Mapper<A, B> {
  B apply(A value);
  default <C> Mapper<A, C> then(Mapper<B, C> next) { return x -> next.apply(apply(x)); }
}
