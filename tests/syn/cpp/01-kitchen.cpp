#include <vector> // header
#include "local.hpp"
#if defined(FLAG) && 12
"macro" /* TODO */
#endif
#define VALUE defined(OTHER) + 1 + "x" /* macro */ // tail
/// API docs
/* FIXME block */
[[outer::tag([[nested]])]] class Box {};
template<typename T> concept Sized = requires(T x) { x.size(); };
constexpr unsigned long value = 0xCA'FEuLL + 0b1010;
bool f(char c, const char *s) { return c == '\n' && s != nullptr && true; }
auto nested = std::vector<std::vector<int>>{};
int ops = value++ + --value; // comment
*/
