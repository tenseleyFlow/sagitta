#define APPLY(x) ((x) * 2)
namespace app {
struct Runner {
  virtual ~Runner() noexcept = default;
  int operator()(int input) const { return APPLY(input); }
};
}
